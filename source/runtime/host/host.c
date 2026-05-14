/*==============================================================================================

    rt_host.c — runtime host implementation.

    Boot sequence:
        1. mod_system_init()
        2. mod_static_load( "sys", ... )        — mandatory; timing + sleep
        3. load every entry in desc->modules
        4. mod_init_all()                       — topo-sort + init in dep order
        5. HOST_FETCH_API( app, render )        — cache host-owned API ptrs
        6. window_open()                        — when app is loaded (inferred from k_modules)
        7. desc->on_ready()                     — host post-init hook
        8. enter loop per desc->loop_mode
        9. window_close() + mod_system_exit()  — exit in reverse dep order

    The loop is intentionally explicit. rt_host.c knows the engine-level modules
    it manages (app, render) and calls them by name. It does not iterate the dep
    graph generically.

    API slot stability
    ------------------
    mod_get_api() returns a pointer to the module's stable api_slot — a block the
    system owns and updates in-place on every hot-reload. g_app_api_ptr and
    g_render_api_ptr cached here never need refreshing; the function pointers
    they point to are live after every reload flush.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"
#include "engine/sys/sys.h"
#include "engine/app/app.h"
#include "runtime_module/render/render_api.h"

#include "runtime/host/host.h"

/*==============================================================================================
    Quit flag (headless path)
==============================================================================================*/

static bool g_quit_requested = false;

void
rt_host_quit( void )
{
    g_quit_requested = true;
}

bool
rt_host_should_quit( void )
{
    return g_quit_requested;
}

/*==============================================================================================
    Cached engine module API pointers

    MOD_DEFINE_API_PTR  — defines g_<name>_api_ptr storage (no-op in BUILD_STATIC)
    HOST_FETCH_API      — populates the pointer after mod_init_all()
                          (no-op in BUILD_STATIC; struct is linked directly)

    In static/monolithic builds:
        app_api() == &g_app_api_struct  — direct address, LTO can devirtualize call sites.

    In dynamic builds:
        app_api() == g_app_api_ptr      — cached by HOST_FETCH_API; NULL if not loaded.
        The null path handles headless hosts that don't include app in their module list.
==============================================================================================*/

MOD_DEFINE_API_PTR( app_api_t,    app    );
MOD_DEFINE_API_PTR( render_api_t, render );

static win_id_t s_win_id = APP_WIN_INVALID;

/*==============================================================================================
    Module loading
==============================================================================================*/

static bool
load_entry( const rt_module_entry_t* e )
{
    return e->get_mod_api ? mod_static_load( e->name, e->get_mod_api() ) : mod_dynamic_load( e->name );
}

static bool
load_all( const rt_module_entry_t* modules )
{
    if ( !modules )
        return true;

    for ( const rt_module_entry_t* e = modules; e->name; ++e )
    {
        if ( load_entry( e ) == false )
        {
            fprintf( stderr, "[rt_host] failed to load '%s': %s\n", e->name, mod_last_error() );
            return false;
        }
    }
    return true; /* all loaded */
}

/*==============================================================================================
    Main entry
==============================================================================================*/

int
rt_host_main( const rt_host_desc_t* desc, int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    if ( !desc || !desc->modules )
    {
        fprintf( stderr, "[rt_host] descriptor or module list is missing\n" );
        return 1;
    }

    g_quit_requested = false;
    s_win_id         = APP_WIN_INVALID;

    /* ---- boot --------------------------------------------------------- */

    mod_system_init();

    /* sys is mandatory — provides tick_reset and sleep for the loop */
    if ( !mod_static_load( "sys", sys_get_mod_api() ) )
    {
        fprintf( stderr, "[rt_host] sys load failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    if ( !load_all( desc->modules ) )
    {
        mod_system_exit();
        return 1;
    }

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[rt_host] init failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    /* ---- cache engine module APIs ------------------------------------- */
    /*
       HOST_FETCH_API in static builds:  no-op — app_api() / render_api() return the
                                         linked struct directly.
       HOST_FETCH_API in dynamic builds: populates g_*_api_ptr from the module registry.
                                         Returns NULL when the module is absent — headless
                                         hosts that don't load app or render get NULL here,
                                         which is fine; the windowed path guards against it.
    */
    HOST_FETCH_API( app_api_t,    app    );
    HOST_FETCH_API( render_api_t, render );

    /* ---- windowed path: inferred from k_modules[] -------------------- */
    /*
       If app was declared in k_modules, app_api() is non-NULL here and we
       create a window. No separate flag — the module list is the declaration.
    */
    const bool windowed = ( app_api() != NULL );

    if ( windowed )
    {
        const i32 w = desc->window_width  > 0 ? desc->window_width  : 1280;
        const i32 h = desc->window_height > 0 ? desc->window_height : 720;

        s_win_id = app_api()->window_open( desc->name ? desc->name : "orb", 0, 0, w, h, APP_WIN_DEFAULT );
        if ( s_win_id == APP_WIN_INVALID )
        {
            fprintf( stderr, "[rt_host] window creation failed\n" );
            mod_system_exit();
            return 1;
        }
    }

    printf( "[rt_host] '%s' ready\n", desc->name ? desc->name : "host" );

    /* ---- optional console input -------------------------------------- */

    const bool console    = ( desc->flags & RT_HOST_CONSOLE    ) != 0;
    const bool hot_reload = ( desc->flags & RT_HOST_HOT_RELOAD ) != 0;
    const i32  frame_ms   = desc->frame_target_ms > 0 ? desc->frame_target_ms : 16;

    if ( console && !sys_console_input_init() )
        fprintf( stderr, "[rt_host] WARNING: console input init failed\n" );

    /* ---- post-init host hook ----------------------------------------- */

    if ( desc->on_ready )
        desc->on_ready();

    /* ---- caller-driven path ------------------------------------------ */

    if ( desc->loop_mode == RT_LOOP_NONE )
        return 0;

    /* ---- loop -------------------------------------------------------- */

    sys_api()->tick_reset();

    while ( !g_quit_requested )
    {
        /* -- pump OS events (windowed) ---------------------------------- */

        if ( windowed && !app_api()->pump_events() )
            break;    /* window closed — exit main loop */

        /* -- dt --------------------------------------------------------- */

        f32 dt = ( f32 )sys_api()->tick_reset();
        if ( dt > 0.25f )
            dt = 0.25f; /* spiral-of-death guard */

        /* -- console key state ------------------------------------------ */

        if ( console )
            sys_console_input_poll();

        /* -- host update ------------------------------------------------- */

        /* Sandbox logic, game bootstrap, tool work. Lives at the top of the
           stack — can call any loaded module API or rt_host_quit(). */

        if ( desc->on_update )
            desc->on_update( dt );

        /* -- render ------------------------------------------------------ */

        if ( windowed && render_api() )
        {
            render_api()->begin_frame();
            render_api()->draw_frame( dt );
            render_api()->end_frame();
        }

        /* -- hot-reload -------------------------------------------------- */

        if ( hot_reload )
        {
            mod_check_reloads();
            mod_system_flush_reloads();
        }

        /* -- single-shot exit -------------------------------------------- */

        if ( desc->loop_mode == RT_LOOP_ONCE )
            break;

        /* -- frame pacing ------------------------------------------------ */

        sys_api()->sleep_milliseconds( frame_ms );
    }

    /* ---- shutdown ---------------------------------------------------- */

    if ( windowed && app_api() && s_win_id != APP_WIN_INVALID )
        app_api()->window_close( s_win_id );

    if ( console )
        sys_console_input_shutdown();

    mod_system_exit();
    return 0;
}

/*============================================================================================*/
/*============================================================================================*/
