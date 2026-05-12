/*==============================================================================================

    sandbox/runtime/sb_runtime_main.c — Runtime stack sandbox.

    Validates the full host stack end-to-end using the new runtime_host abstraction.
    This is the first host that is not hand-rolled — it delegates everything to
    runtime_host_run() and provides only the three thin callbacks.

    Console keys
    ------------
        V  verify cached API pointer is still live after any reload
        R  force-queue all dynamic modules for reload
        Q  quit cleanly

==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "engine/sys/sys.h"
#include "engine/sys/sys_api.h"
#include "engine/mod/mod.h"

#include "host/common/host_common.h"
#include "runtime/host/runtime_host.h"

#include "runtime_module/example/example_api.h"

/* Allocate the cached API pointer for the example module in this translation unit. */
MOD_DEFINE_API_PTR( example_api_t, example );

/*==============================================================================================
    Sandbox State
==============================================================================================*/

typedef struct sb_state_s
{
    i32  frame;
    bool console_ok;

} sb_state_t;

/*==============================================================================================
    Callbacks
==============================================================================================*/

static bool
sb_on_init( void* userdata )
{
    sb_state_t* s = ( sb_state_t* )userdata;

    /* Fetch example API pointer from the now-initialized module system. */
    if ( !HOST_FETCH_API( example_api_t, example ) )
    {
        fprintf( stderr, "[sb_runtime] failed to fetch example API\n" );
        return false;
    }

    /* simply to test that the pointer is valid */
    ORB_ASSERT( example_api() );

    s->console_ok = sys_console_input_init();
    if ( !s->console_ok )
        printf( "[sb_runtime] warning: console input unavailable\n" );

    printf( "\nV = verify API pointer    R = reload all    Q = quit\n\n" );
    return true;
}

static bool
sb_on_update( float dt, void* userdata )
{
    sb_state_t* s = ( sb_state_t* )userdata;

    if ( s->console_ok )
        sys_console_input_poll();

    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[sb_runtime] Q — quitting\n" );
        return false;
    }

    if ( sys_key_pressed( PLATFORM_KEY_V ) )
    {
        printf( "[sb_runtime] V — verifying cached API pointer\n" );
        example_api()->example_function_1();
        example_api()->example_function_2( s->frame );
        printf( "[sb_runtime] pointer still valid\n" );
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[sb_runtime] R — queuing reload for all modules\n" );
        mod_reload_all();
    }

    example_api()->update( dt );

    s->frame++;
    if ( s->frame % 300 == 0 )
        printf( "[sb_runtime] frame %d\n", s->frame );

    return true;
}

static void
sb_on_exit( void* userdata )
{
    sb_state_t* s = ( sb_state_t* )userdata;

    if ( s->console_ok )
        sys_console_input_shutdown();

    printf( "[sb_runtime] exited after %d frames\n", s->frame );
}

/*==============================================================================================
    Module list
==============================================================================================*/

/* RUNTIME_MODULE expands to { "example", example_get_mod_api } in BUILD_STATIC,
   or { "example", NULL } in dynamic builds (runtime_host calls mod_dynamic_load). */

static const rt_module_entry_t k_modules[] = {
    RUNTIME_MODULE( example ),
    RUNTIME_MODULE_END,
};

/*==============================================================================================
    Entry point
==============================================================================================*/

int
main( int argc, char** argv )
{
    launch_params_t params;
    host_args_parse( argc, argv, &params );

    sb_state_t             state  = { 0 };

    const rt_config_t config = {
        .host_name         = "sb_runtime",
        .modules           = k_modules,
        .on_init           = sb_on_init,
        .on_update         = sb_on_update,
        .on_exit           = sb_on_exit,
        .userdata          = &state,
        .frame_target_ms   = 16,
        .enable_hot_reload = true,
    };

    return runtime_host_run( &config ) ? 0 : 1;
}

/*============================================================================================*/