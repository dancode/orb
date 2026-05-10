/*==============================================================================================

    engine_main.c

==============================================================================================*/


#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "orb.h"         // general project settings and declarations
#include "engine/mod/mod.h"    // module system (only for host exe, not modules themselves)

#include "sb_engine_test.c"

/*============================================================================================*/
/* specifiy statically linked modules before including headers */

#define JOBS_STATIC    // static to exe and dynamic for modules if not declared

#include "engine/sys/sys_api.h"
#include "engine/core/core_api.h"

// #include "runtime_modules/example/example_api.h"

/*============================================================================================*/

#ifdef BUILD_STATIC
// #include "runtime_modules/example/example_api.h"
// #include "runtime_modules/render/render_api.h"
// #include "runtime_modules/audio/audio_api.h"
// struct mod_api_s* audio_get_mod_api( void );
// struct mod_api_s* render_get_mod_api( void );
// struct mod_api_s* game_get_mod_api( void );
// struct mod_api_s* sample_game_get_mod_api( void );
#endif

// MOD_DEFINE_API_PTR( example_api_t, example ); /* only in dynamic builds, but no harm in static */

/*============================================================================================*/
/* always statically linked headers */

#include "engine/sys/sys.h"

/*============================================================================================*/

static void
host_force_hot_reload( void )
{
    printf( "[host] force hot reload\n" );

    /*
        Later:
            module_sys_reload_changed_modules();
    */
}

static void
host_force_recompile( void )
{
    printf( "[host] force recompile\n" );

    /*
        Later:
            build_tool_compile_game_code();
    */
}

/*============================================================================================*/
/* test the module system by booting it, registering some static modules,
   loading some dynamic ones, and running a main loop */

void
module_test( void )
{
    /*------------------------------------------------------------------------------------------
        1. Boot
    ------------------------------------------------------------------------------------------*/

    mod_system_init();

    /* ── Tier 1: Host services — explicit mod_static_load, never a DLL ────────────── */

    if ( !mod_static_load( "core", core_get_mod_api() ) )
        goto shutdown;
    if ( !mod_static_load( "sys", sys_get_mod_api() ) )
        goto shutdown;

    /* ── Tier 2: Switchable modules — static or DLL depending on BUILD_STATIC ─────── */

    if ( !mod_load( render ) )
    {
        fprintf( stderr, "load renderer: %s\n", mod_last_error() );
        goto shutdown;
    }
    if ( !mod_load( audio ) )
    {
        fprintf( stderr, "load audio: %s\n", mod_last_error() );
        goto shutdown;
    }
    // if ( !mod_load( game ) )
    // {
    //     fprintf( stderr, "load game: %s\n", mod_last_error() );
    //     goto shutdown;
    // }
    // if ( !mod_load( sample_game ) )
    // {
    //     fprintf( stderr, "load sample_game: %s\n", mod_last_error() );
    //     goto shutdown;
    // }

    /*------------------------------------------------------------------------------------------
        3. Initialize — topo-sort fires audio_init() before renderer_init()
    ------------------------------------------------------------------------------------------*/

    if ( mod_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", mod_last_error() );
        goto shutdown;
    }
    mod_list_all();

    // HOST_FETCJ_API( example_api_t, example );
    core_api()->log( "Module System Initialized\n" );
        
    /* ---- console input ----------------------------------------------- */

    {
        if ( !sys_console_input_init() )
        {
            printf( "[host] failed to initialize console input\n" );
            return;
        }
        printf( "\nBootstrap host running.\n" );
        printf( "Keys:\n" );
        printf( "  R = force hot reload\n" );
        printf( "  C = force recompile\n" );
        printf( "  Q = quit\n" );
        printf( "  ESC = quit\n" );
    }

    /* ---- game loop --------------------------------------------------- */

    const float dt      = 1.0f / 60.0f;

    bool        running = true;
    while ( running )
    {
        // engine->print( "Looping...\n" );
        // sys->tick_sleep( 100 );

        mod_system_tick( dt ); /* tick all INITIALIZED modules        */

        /* Application-level API calls — no #ifdefs here, ever. */
        // render_api()->draw_frame( dt );
        // renderer_api()->draw_quad( 0.0f, 0.0f, 100.0f, 100.0f, 0xFF0000FF );
        // renderer_api()->draw_quad( 200.0f, 150.0f, 50.0f, 50.0f, 0x00FF00FF );
        // renderer_api()->end_frame();

        if ( 0 )
        {
            // engine_request_quit();
        }

        sys_console_input_poll();

        if ( sys_key_pressed( PLATFORM_KEY_R ) )
        {
            printf( "[host] R key pressed\n" );
            // hot_reload();
        }

        if ( sys_key_pressed( PLATFORM_KEY_C ) )
        {
            printf( "[host] C key pressed\n" );
            // recompile_code();
        }

        if ( sys_key_pressed( PLATFORM_KEY_Q ) )
        {
            printf( "[host] Q key pressed\n" );
            running = false;
        }

        mod_check_reloads(); /* poll disk; reload any changed DLL   */
    }

    sys_console_input_shutdown();

    /* ------------------------------------------------------------------ */
    /* 6. Shutdown: exit in reverse dep order, unload DLLs, free state    */
    /* ------------------------------------------------------------------ */

shutdown:

    fprintf( stderr, "%s\n", mod_last_error() );
    mod_system_exit();
}

/*============================================================================================*/
/* main entry point */

void test( int argc, char** argv );    // <-- test entry point, defined in engine_test.c

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    module_test();

    if ( 0 )
    {
        test( argc, argv );
    }

    return 0;
}

/*============================================================================================*/