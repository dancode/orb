/*==============================================================================================

    engine/job/job_api.c — Platform-agnostic job module wiring.

==============================================================================================*/
#include "engine/mod/mod_export.h"
#include "engine/job/job_api.h"

/*==============================================================================================
    API struct
==============================================================================================*/

const job_api_t g_job_api_struct = {
    .dispatch = job_dispatch,
    .wait     = job_wait,
    .tick     = job_tick,
};

/*==============================================================================================
    Module lifecycle
==============================================================================================*/

static bool
job_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    return true;
}

static void
job_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
job_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( job_api_t ),
        .func_api      = ( void* )&g_job_api_struct,
        .dep_count     = 0,
        .deps          = {},
        .init          = job_mod_init,
        .exit          = job_mod_exit,
        .reload        = NULL,
    };
    return &api;
}
