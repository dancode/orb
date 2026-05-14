/*==============================================================================================

    sandbox/sb_engine_mod.c — For testing module library.

    Not a real host; just a place to call app APIs and verify they work.

    Boots the module system, loads the example module (static or dynamic per build mode),
    and runs a main loop exercising hot-reload, failure/rollback, and cached API access.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_api.h"
#include "engine/sys/sys.h"

#include "developer/dev_hot/dev_hot.h"

#include "runtime_module/example/example_api.h"
MOD_DEFINE_API_PTR( example_api_t, example );

/*============================================================================================*/

void
module_test( void )
{
    mod_system_init();

    dev_hot_init( NULL, NULL );

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

    HOST_FETCH_API( example_api_t, example );
    example_api()->example_function_1();

    /* ---- console input ----------------------------------------------- */

    if ( !sys_console_input_init() )
    {
        printf( "[host] failed to initialize console input\n" );
        return;
    }

    printf( "\nKeys:\n" );
    printf( "  C = recompile + reload example\n" );
    printf( "  R = reload all modules\n" );
    printf( "  F = simulate reload FAILURE (tests rollback)\n" );
    printf( "  V = verify cached api pointer is still live\n" );
    printf( "  Q = quit\n" );

    /* ---- main loop --------------------------------------------------- */

    const float dt      = 1.0f / 60.0f;
    bool        running = true;

    /* Re-fetch after each flush so the pointer stays current post-reload.
    In a real host this belongs in the post-flush update pass, not every tick. */

    HOST_FETCH_API( example_api_t, example );

    while ( running )
    {
        /* --- input ----------------------------------------------------- */

        sys_console_input_poll();

        if ( sys_key_pressed( PLATFORM_KEY_C ) )
        {
            printf( "[host] C: recompile + reload example\n" );
            dev_hot_recompile( "example" );
        }
        if ( sys_key_pressed( PLATFORM_KEY_R ) )
        {
            printf( "[host] R: reload all\n" );
            dev_hot_reload_all();
        }
        if ( sys_key_pressed( PLATFORM_KEY_F ) )
        {
            printf( "[host] F key pressed - arming reload failure\n" );
            example_api()->fail_next_reload();

            bool reload_ok = dev_hot_reload( "example" );
            printf( "[host] dev_hot_reload returned %s (expected: false)\n", reload_ok ? "true" : "false" );

            /* If snapshot_rollback worked, the old DLL is still loaded and these calls succeed.
               Without the snapshot_rollback fix, this would dereference an unloaded library. */
            printf( "[host] post-rollback sanity check - calling example_function_1():\n" );
            example_api()->example_function_1();
            example_api()->example_function_2( 42 );
        }
        if ( sys_key_pressed( PLATFORM_KEY_V ) )
        {
            printf( "[host] V key pressed - calling cached example_api()\n" );
            example_api()->example_function_1();
            example_api()->example_function_2( 99 );
            printf( "[host] cached pointer still valid\n" );
        }
        if ( sys_key_pressed( PLATFORM_KEY_Q ) )
        {
            printf( "[host] Q key pressed\n" );
            running = false;
        }

        /* --- simulation ------------------------------------------------ */

        example_api()->update( dt );

        /* --- file-watch detection (queues debounced reloads) ----------- */

        mod_check_reloads(); /* polls file timestamps, queues any changed modules for reload */

        /* --- frame boundary: apply queued swaps ------------------------ */

        mod_system_flush_reloads();

        /* --- frame pacing ---------------------------------------------- */

        sys_sleep_milliseconds( 16 );
    }

    sys_console_input_shutdown();

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