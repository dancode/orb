/*==============================================================================================

    engine_main.c

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "orb.h"

#include "core/core.h"
#include "module/module.h"
#include "platform_sys/platform_sys.h"

// temp
#include "core_api.h"
#include "platform_sys/platform_sys_api.h"
#include "engine_api.h"

#include "systems/render/render_api.h"

/*============================================================================================*/

int  intern_test( void );                        // ... temporary code ...
void reflection_test( void );                    // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/

/* declared in core_module.c / engine_module.c */
// void engine_module_register( void );

// void platform_app_module_register( void );
// void input_module_register( void );
// void jobs_module_register( void );
// void time_module_register( void );


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
    sys_tick_init();
    sid_init();

    /* get static API pointers for the modules to use during init() */

    // core_api_t*     core     = core_get_api();
    // platform_api_t* platform = platform_get_api();
    // engine_api_t*   engine   = engine_get_api();

    // engine->print( "Module System Test\n" );

    /* ---- boot -------------------------------------------------------- */

    module_system_init();

    /* ---- services (static, registered directly) ---------------------- */

    module_register_static( "core", core_get_module_api(), core_get_api() );
    module_register_static( "platform_sys", platform_sys_get_module_api(), platform_sys_get_api() );
    module_register_static( "engine", engine_get_module_api(), engine_get_api() );

    // service_register( "platform_app", platform_app_get_module_api(), platform_app_get_api() );
    // service_register( "input", input_get_module_api(), input_get_api() );
    // service_register( "jobs", jobs_get_module_api(), jobs_get_api() );
    // service_register( "time", time_get_module_api(), time_get_api() );

    // platform_app_module_register(); /* window, gpu surface              */
    // input_module_register();        /* action mapping on raw events      */
    // jobs_module_register();         /* thread pool, fiber scheduler      */
    // time_module_register();         /* frame dt, timers, fixed timestep  */

    /* ---- systems (dynamic, hot-reloadable DLLs) ---------------------- */

    // system_load( "renderer" );
    // system_load( "audio" );
    // system_load( "physics" );
    // system_load( "animation" );
    // system_load( "game_framework" );
    // system_load( "my_game" );

    if ( module_dynamic_load( "render" ) == false )
    {
        // core->log( "[main] fatal: %s", module_last_error() );
        goto shutdown;
    }

    // if ( module_load( "game" ) == false )
    // {
    //     // core->log( "[main] fatal: %s", module_last_error() );
    //     goto shutdown;
    // }
    //
    // if ( module_load( "sample_game" ) == false )
    // {
    //     // core->log( "[main] fatal: %s", module_last_error() );
    //     goto shutdown;
    // }

    /* ---- init all in dep order --------------------------------------- */

    if ( module_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", module_last_error() );
        goto shutdown;
    }

    module_list_all();

    /* ---- game loop --------------------------------------------------- */

    // engine_api_t* engine = module_get_api( "engine" );
    platform_sys_api_t* platform_sys = module_get_api( "platform_sys" );

    /* test constant API access across modules */
    const render_api_t* r = module_get_api( "renderer" );
    r->draw_frame( 0.5f );


    /*
    platform_app_api_t* platform = module_get_api( "platform_app" );
    while ( !platform->should_quit() )
    {
        module_check_reloads();
        module_system_tick( platform->frame_dt() );
    }
    */

    /* ---- console input ----------------------------------------------- */

    {
        if ( !sys_console_input_init() )
        {
            printf( "[host] failed to initialize console input\n" );
            return;
        }
        printf( "Bootstrap host running.\n" );
        printf( "Keys:\n" );
        printf( "  R = force hot reload\n" );
        printf( "  C = force recompile\n" );
        printf( "  Q = quit\n" );
        printf( "  ESC = quit\n" );
    }

    /* ---- game loop --------------------------------------------------- */

    const float dt = 1.0f / 60.0f;

    // assert( engine );
    assert( platform_sys );

    bool running = true;
    while ( 1 )
    {
        // engine->print( "Looping...\n" );
        platform_sys->tick_sleep( 100 );

        if ( 1 )
        {
            // engine_request_quit();
        }

        sys_console_input_poll();

        if ( sys_key_pressed( PLATFORM_KEY_R ) )
        {
            // hot_reload();
        }

        if ( sys_key_pressed( PLATFORM_KEY_C ) )
        {
            // recompile_code();
        }

        if ( sys_key_pressed( PLATFORM_KEY_Q ) )
        {
            running = false;
        }


        module_check_reloads();   /* poll disk; reload any changed DLL   */
        module_system_tick( dt ); /* tick all INITIALIZED modules        */
    }

    sys_console_input_shutdown();

    /* ------------------------------------------------------------------ */
    /* 6. Shutdown: exit in reverse dep order, unload DLLs, free state    */
    /* ------------------------------------------------------------------ */

shutdown:

    module_system_exit();
    sid_exit();
    sys_tick_exit();
}

/*============================================================================================*/
/* test entry point */

static void
test( int argc, char** argv )
{
    core_api_init();    // <-- required to debug natvis and must go first

    /**************************************************************/
    /* test module system */

    module_test();    // <-- test module system

    if ( 1 )
        return;

    /**************************************************************/
    /* test memory, string intern, and reflection systems */

    if ( 1 )
    {
        mem_test();           // <-- test memory system
        intern_test();        // <-- test string interning system
        reflection_test();    // <-- test reflection system
    }

    /**************************************************************/
    /* test cvar system */

    if ( 1 )
    {
        cvar_system_init();
        test_core_cvar( argc, argv );    // <-- test cvar system
        cvar_system_exit();
    }

    /**************************************************************/

    core_api_exit();
}

/*============================================================================================*/
/* main entry point */

int
main( int argc, char** argv )
{
    test( argc, argv );

    core_init();

    /**************************************************************/
    /* setup module base path -- different per platform */
    /* this setup code is designed to allow Google jules to find library files */

    // ensure_root();

    // sid_t game_module_name = sid_intern_cstr( "sample_game" );
    // ( void )game_module_name;

    // const char* mod_name = "sample_game";
    // char        path[ 256 ];
    //
    // snprintf( path, sizeof( path ), "%s%s%s%s", module_get_base_path(), LIB_PREFIX, mod_name, LIB_EXT );

    /**************************************************************/
    /* new module laoding code path */

    /**************************************************************/
    /*
    if ( 0 )
    {
        struct module_t* game = module_load( mod_name, path );
        if ( game == NULL )
        {
            return 1;
        }

        for ( int i = 0; i < 3; i++ )
        {
            module_call_tick( game );
        }

        module_reload( game );

        for ( int i = 0; i < 2; i++ )
        {
            module_call_tick( game );
        }

        module_unload( game );

        core_exit();
    }
    */
    return 0;
}

/*============================================================================================*/