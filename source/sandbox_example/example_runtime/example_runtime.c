/*==============================================================================================

    sandbox/sandbox_example/example_runtime/example_runtime.c — example runtime host.

    This is a starting point for new runtime hosts: A template used to test runtime 
    functionality such as services and modules.

    Loop:  RT_LOOP_RUN
    Flags: RT_HOST_CONSOLE | RT_HOST_HOT_RELOAD
   
==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "engine/sys/sys.h"
#include "engine/mod/mod.h"

#include "runtime/host/host.h"

/* declare dynamic modules api */
// #include "runtime_module/example/example_api.h"
// MOD_DEFINE_API_PTR( example_api_t, example );

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
server_ready( void )
{
    /* called once after init — fetch APIs for every module we drive in on_update */
    HOST_FETCH_API( example_api_t, example );
}

static void
server_update( f32 dt )
{
    /* Q — clean shutdown (headless quit path; no window close button) */
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[server] shutdown requested\n" );
        rt_host_quit();
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
        example_api()->fail_next_reload();

    /* V — verify the cached API pointer is still live */
    if ( sys_key_pressed( PLATFORM_KEY_V ) )
    {
        printf( "[server] verify - calling example\n" );
        example_api()->example_function_1();
    }

    example_api()->update( dt );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

// RT_MODULE( example ), 
static const rt_module_entry_t k_modules[] = { { 0 } };

static const rt_host_desc_t    k_desc      = {
            .name            = "sandbox_server",
            .flags           = RT_HOST_CONSOLE | RT_HOST_HOT_RELOAD,
            .loop_mode       = RT_LOOP_RUN,
            .frame_target_ms =  33, /* ~30 Hz — typical dedicated server tick */
            .modules         = k_modules,
            .on_ready        = server_ready,
            .on_update       = server_update,
};

int
main( int argc, char** argv )
{
    return rt_host_main( &k_desc, argc, argv );
}
