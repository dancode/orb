/*==============================================================================================

    engine_main.c

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"

// #if PLATFORM_WINDOWS
// #    define WIN32_LEAN_AND_MEAN
// #    include <windows.h>
// #else
// #    include <unistd.h>
// #    include <libgen.h>
// #endif

#include "core/core.h"
#include "core/module_system.h"
#include "platform/platform.h"

// temp
#include "core_api.h"

/*============================================================================================*/

int  intern_test( void );                        // ... temporary code ...
void reflection_test( void );                    // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/

/* declared in core_module.c / engine_module.c */
void core_module_register( void );
void platform_module_register( void );
void engine_module_register( void );

/*============================================================================================*/

void
module_test( void )
{
    sys_tick_init(); 
    sid_init();

    /* get static API pointers for the modules to use during init() */

    core_api_t*     core     = core_get_api();
    platform_api_t* platform = platform_get_api();
    engine_api_t*   engine   = engine_get_api();

    engine->print( "Module System Test\n" );

    /* ------------------------------------------------------------------ */
    /* 1. Boot the module system                                          */
    /* ------------------------------------------------------------------ */

    module_system_init( core, engine );

    /* ------------------------------------------------------------------ */
    /* 2. Register static modules (already live in the exe)               */
    /*    Order matters here only for readability — the topo-sort handles */
    /*    the actual initialization order.                                */
    /* ------------------------------------------------------------------ */

    core_module_register();
    platform_module_register();
    engine_module_register();
    
    /* ------------------------------------------------------------------ */
    /* 3. Load dynamic modules (DLL copy → resolve exports → alloc state) */
    /* ------------------------------------------------------------------ */

    if ( module_load( "render" ) == false )
    {
        core->log( "[main] fatal: %s", module_last_error() );
        goto shutdown;
    }

    if ( module_load( "sample_game" ) == false )
    {
        core->log( "[main] fatal: %s", module_last_error() );
        goto shutdown;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Resolve dependencies, call every init() in order:               */
    /*    core → engine → render → sample_game                            */
    /* ------------------------------------------------------------------ */

    if ( module_init_all() == false )
    {
        core->log( "[main] fatal: %s\n", module_last_error() );
        goto shutdown;
    }

    module_list_all();

    /* ------------------------------------------------------------------ */
    /* 5. Main loop                                                       */
    /* ------------------------------------------------------------------ */

    const float dt = 1.0f / 60.0f;

    while ( engine->should_quit() == false )
    {
        // 1. Get delta time using your sys_tick functions
        // f64 tick = sys_tick_seconds();

        /* sleep to avoid busy-looping; adjust as needed for your timing method */
        platform->tick_sleep( 16 );

        // 2. Do work
        engine->print( "Looping...\n" );

        // sys_tick_sleep( 100 );

        // 3. Logic to quit (Example)
        if ( 1 )
        {
            // engine_request_quit();
        }

        module_check_reloads();   /* poll disk; reload any changed DLL   */
        module_system_tick( dt ); /* tick all INITIALIZED modules        */
    }

    /* ------------------------------------------------------------------ */
    /* 6. Shutdown: exit in reverse dep order, unload DLLs, free state    */
    /* ------------------------------------------------------------------ */

shutdown:

    module_system_exit();
    core_api_exit();
    sid_exit();
    sys_tick_exit();
}

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