/*==============================================================================================

    sandbox_server_main.c — DEDICATED SERVER shape.

    Headless long-running host. Hot-reload lets modules be swapped without restart.
    Console input provides the operator quit path: Q exits cleanly.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "engine/sys/sys.h"
#include "engine/mod/mod_host.h"
#include "runtime/host.h"

/* declare dynamic modules api */
#include "runtime_modules/example/example.h"
MOD_DEFINE_API_PTR( example_api_t, example );

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
server_ready( void )
{
    /* called once after init — fetch APIs for every module we drive in on_update */
    MOD_HOST_FETCH_API( example_api_t, example );
}

static void
server_update( f32 dt )
{
    /* Q — clean shutdown (headless quit path; no window close button) */
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[server] shutdown requested\n" );
        run_host_quit();
        return;
    }

    /* R — force-reload all dynamic modules */
    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[server] reload all\n" );
        mod_reload_all();
    }

    /* F — arm example to fail its next reload (tests rollback) */
    if ( sys_key_pressed( PLATFORM_KEY_F ) )
        example()->fail_next_reload();

    /* V — verify the cached API pointer is still live */
    if ( sys_key_pressed( PLATFORM_KEY_V ) )
    {
        printf( "[server] verify - calling example\n" );
        example()->example_function_1();
    }

    example()->update( dt );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = { RUN_MODULE( example ), { 0 } };

static const run_host_desc_t    k_desc      = {
            .name            = "sandbox_server",
            .flags           = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD,
            .loop_mode       = RUN_LOOP_RUN,
            .frame_target_ms = 33, /* ~30 Hz — typical dedicated server tick */
            .modules         = k_modules,
            .on_ready        = server_ready,
            .on_update       = server_update,
};

int
main( int argc, char** argv )
{
    return run_host_main( &k_desc, argc, argv );
}

/*============================================================================================*/