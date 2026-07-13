#ifndef JOB_API_H
#define JOB_API_H
/*==============================================================================================

    engine/job/job_api.h — job module API struct and gateway macro.

    Consumers call job()->dispatch(...) etc.

==============================================================================================*/

#include "engine/job/job.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct job_api_s
{
    /* Dispatch a set of parallel jobs. Returns a counter handle that can be awaited. */
    job_counter_t ( *dispatch )( const job_decl_t* decls, uint32_t count );

    /* Wait for a job group to finish. Handles stale/null counters safely. */
    void          ( *wait )( job_counter_t counter );

    /* Frame-level dispatch function. Used by runtime host in main loop. */
    void           ( *tick )( void );

} job_api_t;

/*============================================================================================*/

/* job is an opt-in service (like app / net): a host that schedules no work never loads it,
   so the worker pool is not spun up.  Under MOD_HOST_DYNAMIC_SERVICES (the runtime unity)
   job() is the runtime pointer gateway -- NULL when the module is absent -- so the host's
   if ( job() ) tick guard is live.  Everywhere else it keeps the direct static gateway. */
#if ( defined( BUILD_STATIC ) || defined( JOB_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    MOD_GATEWAY_STATIC( job_api_t, job )
    #define MOD_USE_JOB
    #define MOD_FETCH_JOB  true
#else
    MOD_GATEWAY_DYNAMIC( job_api_t, job )
    #define MOD_USE_JOB    MOD_DEFINE_API_PTR( job_api_t, job )
    #define MOD_FETCH_JOB  MOD_FETCH_API( job_api_t, job )
#endif

// clang-format on
/*============================================================================================*/
#endif    // JOB_API_H
