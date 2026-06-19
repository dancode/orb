/*==============================================================================================

    engine/jobs/jobs_api.c — Platform-agnostic jobs module wiring.

==============================================================================================*/
#include "engine/mod/mod_export.h"
#include "engine/jobs/jobs_api.h"

/*==============================================================================================
    API struct
==============================================================================================*/

const jobs_api_t g_jobs_api_struct = {
    .dispatch = jobs_dispatch,
    .wait     = jobs_wait,
    .tick     = jobs_tick,
};

/*==============================================================================================
    Module lifecycle
==============================================================================================*/

static bool
jobs_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    return true;
}

static void
jobs_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
jobs_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( jobs_api_t ),
        .func_api      = ( void* )&g_jobs_api_struct,
        .dep_count     = 0,
        .deps          = {},
        .init          = jobs_mod_init,
        .exit          = jobs_mod_exit,
        .reload        = NULL,
    };
    return &api;
}
