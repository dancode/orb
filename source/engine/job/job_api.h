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
    /* Size the worker pool. Frontend policy, called once after load: worker_count < 0 = auto
       (one per core minus main thread), 0 = no workers (main thread is the pool), > 0 = that
       many. Loading the module spawns nothing; this is what creates threads (if any). */
    void          ( *configure )( i32 worker_count );

    /* Dispatch a set of parallel jobs. Returns a counter handle that can be awaited. */
    job_counter_t ( *dispatch )( const job_decl_t* decls, uint32_t count );

    /* Wait for a job group to finish. Handles stale/null counters safely. */
    void          ( *wait )( job_counter_t counter );

    /* Frame-level dispatch function. Used by runtime host in main loop. */
    void           ( *tick )( void );

} job_api_t;

/*============================================================================================*/

/* job is part of the engine floor -- always loaded, so it keeps the direct static gateway
   (job() is always valid, never NULL). Its worker pool is what's optional, not the module:
   job_configure(0) loads it with zero threads. */
#if defined( BUILD_STATIC ) || defined( JOB_STATIC )
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
