#ifndef JOBS_API_H
#define JOBS_API_H
/*==============================================================================================

    engine/jobs/jobs_api.h — jobs module API struct and gateway macro.

    Consumers call jobs()->dispatch(...) etc.

==============================================================================================*/

#include "engine/jobs/jobs.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct jobs_api_s
{
    /* Dispatch a set of parallel jobs. Returns a counter that can be awaited. */
    job_counter_t* ( *dispatch )( const job_decl_t* decls, uint32_t count );

    /* Wait for a job counter to reach zero. */
    void           ( *wait )( job_counter_t* counter );

    /* Frame-level dispatch function. Used by runtime host in main loop. */
    void           ( *tick )( void );

} jobs_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( JOBS_STATIC )
    MOD_GATEWAY_STATIC( jobs_api_t, jobs )
#else
    MOD_GATEWAY_DYNAMIC( jobs_api_t, jobs )
#endif

#if defined( BUILD_STATIC ) || defined( JOBS_STATIC )
    #define MOD_USE_JOBS
    #define MOD_FETCH_JOBS  true
#else
    #define MOD_USE_JOBS    MOD_DEFINE_API_PTR( jobs_api_t, jobs )
    #define MOD_FETCH_JOBS  MOD_FETCH_API( jobs_api_t, jobs )
#endif

// clang-format on
/*============================================================================================*/
#endif    // JOBS_API_H
