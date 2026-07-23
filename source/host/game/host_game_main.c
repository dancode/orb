/*==============================================================================================

    host_game_main.c -- the "game.exe" host: engine + runtime + PROJECT GAME DLL.

    The runtime shape of the modular architecture: this exe owns the window, the rhi
    context, and the loop (run_host_main); the GAME is a project DLL selected at launch
    and loaded by the runtime at boot:

        host_game.exe -project <dir>                 loads <dir>/bin/<name>.dll (name = dir basename)
        host_game.exe -module sample_game            loads sample_game.dll from the exe dir
        host_game.exe -project <dir> -module <name>  -module overrides the basename

    The project implements runtime/run_project.h, and the game framework runner drives
    it -- this host is pure POLICY: bind + play at ready, one game()->tick( dt, view )
    per frame (fixed-step sim, pacing, and the play session live in the runner), stop on
    close.  The view hands the project its render target and surface size every frame.

    Dev keys Q/R read the CONSOLE (terminal focus only) so they can't be fumbled from the
    game window -- same policy as host_editor.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include "orb.h"

#include "engine/sys/sys_host.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/gui/gui_host.h"
#include "runtime_service/console/console_host.h"
#include "runtime_modules/render/render_api.h"
#include "game/game_api.h"
#include "runtime/run_project.h"

#include "runtime/run_api.h"
#include "runtime/run_host.h"

#include "host/common/host_common.h"

MOD_USE_APP;
MOD_USE_RUN;
MOD_USE_GAME;

#if defined( BUILD_STATIC ) && defined( HOST_PROJECT )
/* Ship shape: this exe is a per-project monolithic target (orb.targets: define
   HOST_PROJECT=<name> + mono_dep <name>).  The project is compiled in as a static lib and
   registered through run_host_desc_t.project_get_mod_desc -- exactly one project, no DLL
   on disk, hot-reload a no-op.  Plain host_game (no define) is untouched by this block. */
mod_desc_t* HOST_XPASTE( HOST_PROJECT, _get_mod_desc )( void );
#define HOST_PROJECT_NAME HOST_XSTR( HOST_PROJECT )
#endif

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static host_project_t s_proj;    /* resolved -project/-module */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
game_host_ready( void )
{
    /* Hand the project to the runner: bind resolves its stable api slot (live across
       hot-reloads), play starts the session.  This host's policy is "play at boot". */
    if ( !MOD_HOST_FETCH_API( game ) || !game()->project_bind( s_proj.name ) )
    {
        fprintf( stderr, "[host_game] project '%s' has no api\n", s_proj.name );
        run_host_quit();
        return;
    }

    printf( "[host_game] running project '%s'\n", s_proj.name );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all\n" );

    game()->play();
}

static void
game_handle_shortcuts( void )
{
    /* Developer hotkeys -- CONSOLE input on purpose (terminal focus only; can't be
       fumbled from the game window).  See host_editor_main.c for the full rationale. */
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[host_game] Q -- quit\n" );
        game()->stop();
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[host_game] R -- reload all\n" );
        mod_reload_all();
    }
}

static void
game_host_update( f32 dt )
{
    game_handle_shortcuts();

    /* The one per-frame runner call.  The view is rebuilt every frame -- surface size
       tracks resizes and a hot-reloaded project can never hold a stale handle.  The
       runner owns everything else: fixed-step sim, interpolation alpha, play state. */

    run_view_t view = {
        .version    = RUN_VIEW_VERSION,
        .render_ctx = run_host_ctx(),
        .gui_vp     = -1,   /* tick phase: gui emission is illegal here (see on_hud) */
    };
    app()->window_get_size( run_host_window(), &view.surface_w, &view.surface_h );
    game()->tick( dt, &view );

    /* A live session's HUD changes every frame (score, tick meter) and the retained-cache
       emit skip would freeze it -- the same force-redraw gate host_editor holds while a
       session is live.  Re-derived every frame so it clears when the session ends. */
    gui()->set_force_redraw( game()->state() != GAME_STOPPED );
}

/* The project's HUD phase.  This callback runs inside run_host's gui frame bracket -- the
   only place widget emission is legal -- so this is where the runner forwards on_hud.  The
   view is rebuilt here too (stale-handle rule); gui_vp hands the project the main viewport. */

static void
game_gui( f32 dt )
{
    gui_vp_t vp = run_host_vp();

    run_view_t view = {
        .version    = RUN_VIEW_VERSION,
        .render_ctx = run_host_ctx(),
        .gui_vp     = ( vp == GUI_VP_INVALID ) ? -1 : ( i32 )vp,
    };
    app()->window_get_size( run_host_window(), &view.surface_w, &view.surface_h );
    game()->hud( dt, &view );
}

/* Window X pressed: stop the session, then allow the close.  (Q-quit stops it too; the
   project module's own exit() during mod_system_exit is the backstop either way.) */

static bool
game_host_close_request( void )
{
    game()->stop();
    return true;
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( rhi     ),   /* GPU backend -- static service                 */
    RUN_SERVICE( draw    ),   /* immediate primitives -- render's draw backend */
    RUN_SERVICE( gui     ),   /* immediate mode GUI -- menus, HUD, dev console */
    RUN_SERVICE( console ),   /* dev console drop-down -- gui front end over core */
    RUN_MODULE ( render  ),   /* scene frame owner                             */
    RUN_MODULE ( game    ),   /* game framework runner -- drives project DLLs   */
    { 0 }
};

/* gui composites over render's scene (the same path host_editor uses).  A font is required
   for the console (and any menu/HUD) to render text -- GUI_FONT_NONE would draw nothing. */
static const run_gui_desc_t k_gui_desc = {
    .font  = GUI_FONT_ROBOTO_16,
    .clear = { 0.0f, 0.0f, 0.0f, 0.0f },   /* alpha 0 = render owns the clear (path A) */
    .debug = false,
};

int
main( int argc, char** argv )
{
    /* Launch shapes: see the cheat sheet in host_common.h. */
    launch_params_t params;
    host_args_parse( argc, argv, &params );

#if defined( BUILD_STATIC ) && defined( HOST_PROJECT )
    /* Ship path: the project is compiled in, not on disk -- host_resolve_project's
       missing-dll check would hard-error, so it is skipped.  Args may only confirm the
       baked project, never select another. */
    if ( params.project_path[ 0 ] ||
         ( params.module_override[ 0 ] &&
           strcmp( params.module_override, HOST_PROJECT_NAME ) != 0 ) )
    {
        fprintf( stderr, "[host_game] ship build carries one project ('%s'); "
                         "-project/-module cannot select another\n", HOST_PROJECT_NAME );
        return 1;
    }
    snprintf( s_proj.name, sizeof( s_proj.name ), "%s", HOST_PROJECT_NAME );
    s_proj.dir[ 0 ] = '\0';
    s_proj.present  = true;
#else
    char err[ 512 ];
    if ( !host_resolve_project( &params, &s_proj, err, sizeof( err ) ) )
    {
        fprintf( stderr, "[host_game] %s\n", err );
        return 1;
    }
    if ( !s_proj.present )
    {
        fprintf( stderr, "usage: host_game.exe -project <dir> [-module <name>] [-dev]\n"
                         "       host_game.exe -module <name>\n" );
        return 1;
    }
#endif

    u32 flags = RUN_HOST_WINDOWED | RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD;

    const run_host_desc_t desc = {
        .name             = s_proj.name,
        .flags            = flags,
        .loop_mode        = RUN_LOOP_RUN,
        .window_width     = 1280,
        .window_height    = 720,
        .modules          = k_modules,
        .gui              = &k_gui_desc,
        .project_name     = s_proj.name,
        .project_dir      = s_proj.dir,
#if defined( BUILD_STATIC ) && defined( HOST_PROJECT )
        .project_get_mod_desc = HOST_XPASTE( HOST_PROJECT, _get_mod_desc ),
#endif
        .on_ready         = game_host_ready,
        .on_update        = game_host_update,
        .on_gui           = game_gui,
        .on_close_request = game_host_close_request,
    };

    return run_host_main( &desc, argc, argv );
}

// clang-format on
/*============================================================================================*/
