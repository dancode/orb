/*==============================================================================================

    engine/job/job_api.c — Platform-agnostic job module wiring.

==============================================================================================*/
#include "engine/mod/mod_export.h"
#include "engine/job/job_api.h"

/*==============================================================================================
    API struct
    
    This is the public interface table exposed to the rest of the engine.
    Other modules retrieve this pointer and call these functions (e.g. job()->dispatch(...))
    without needing to know the underlying implementation details.
==============================================================================================*/

const job_api_t g_job_api_struct = 
{
    .dispatch = job_dispatch, // Triggers parallel task execution.
    .wait     = job_wait,     // Synchronously waits for tasks to finish.
    .tick     = job_tick,     // Standard per-frame update tick (unused in Tier I).
};

/*==============================================================================================
    Module lifecycle
    
    These callbacks are invoked automatically by ORB's module registry manager:
    - job_mod_init: Runs when the job module is first loaded. Set up worker threads here.
    - job_mod_exit: Runs when the engine shuts down. Joins and cleans up worker threads.
==============================================================================================*/

static bool
job_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    // Delegates actual memory and thread pool initialization to job_init in job.c.
    return job_init( raw_state );
}

static void
job_mod_exit( void* raw_state )
{
    // Cleans up active OS threads and handles during module shutdown in job.c.
    job_exit( raw_state );
}

/*==============================================================================================
    Module descriptor
    
    This structure tells ORB's loader how to manage the job module:
    - state_size: Informs the system to allocate and preserve 'job_state_t' bytes of memory.
    - func_api: Points to the static interface struct defined above.
    - init/exit: Registration of startup and shutdown hooks.
==============================================================================================*/

mod_desc_t*
job_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( job_state_t ), // System allocates this state block for us.
        .func_api_size = sizeof( job_api_t ),
        .func_api      = ( void* )&g_job_api_struct,
        .dep_count     = 1,
        .deps          = { "sys" },
        .init          = job_mod_init,
        .exit          = job_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/