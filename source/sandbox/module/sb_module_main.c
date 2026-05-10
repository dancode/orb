/*==============================================================================================

    engine_main.c

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/sys/sys_api.h"
#include "engine/sys/sys.h"    // static inline functions for console input 

#include "developer/dev_build_invoker/build_invoker.h"

// #include "runtime_module/example/example_api.h"

/*============================================================================================*/

static void
host_force_hot_reload( void )
{
    printf( "[host] force hot reload\n" );
    int reloaded_count = mod_reload_all();
    printf( "[host] force reload: %d module(s) reloaded\n", reloaded_count );
}

static void
host_force_recompile( void )
{
    printf( "[host] force recompile\n" );

    // mod_unload( "example" ); /* unload the module before recompiling, to free the DLL on Windows */ 

    dev_build_result_t r;
    if ( !dev_build_module( "example", &r ) )
    {
        /* fallback: try to reload the old module if the build system couldn't even be launched */
        // mod_load( example );    
        fprintf( stderr, "[host] could not launch build: %s\n", dev_build_last_error() );
        return;
    }

    /* fallback: try to reload the old module if the build failed (e.g. due to compile errors) */
    // mod_load( example ); 

    if ( !r.success )
    {
        fprintf( stderr, "[host] build FAILED (exit %d, %.2fs):\n%.*s\n", r.exit_code, r.elapsed_seconds,
                 r.log_len, r.log );
        return;
    }

    printf( "[host] build OK in %.2fs - reloading.\n", r.elapsed_seconds );
    mod_reload( "example" );
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
    dev_build_init( NULL ); /* auto-detect cmake on PATH and build dir from exe location */

    if ( !mod_static_load( "sys", sys_get_mod_api() ) )
        goto shutdown;

    if ( !mod_load( example ) )
    {
        fprintf( stderr, "load example: %s\n", mod_last_error() );
        goto shutdown;
    }

    if ( mod_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", mod_last_error() );
        goto shutdown;
    }

    mod_list_all();
    
    // HOST_FETCJ_API( example_api_t, example );
    
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

    const float dt = 1.0f / 60.0f;

    UNUSED( dt );

    bool running = true;
    while ( running )
    {
        sys_sleep_milliseconds( 16 );

        mod_system_tick( dt ); /* tick all INITIALIZED modules */

        sys_console_input_poll();
        
        if ( sys_key_pressed( PLATFORM_KEY_R ) )
        {
            printf( "[host] R key pressed\n" );
            host_force_hot_reload();
        }
        
        if ( sys_key_pressed( PLATFORM_KEY_C ) )
        {
            printf( "[host] C key pressed\n" );
            host_force_recompile();
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

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    module_test();
    return 0;
}

/*============================================================================================*/