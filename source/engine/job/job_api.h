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
    /* Dispatch a set of parallel jobs. Returns a counter that can be awaited. */
    job_counter_t* ( *dispatch )( const job_decl_t* decls, uint32_t count );

    /* Wait for a job counter to reach zero. */
    void           ( *wait )( job_counter_t* counter );

    /* Frame-level dispatch function. Used by runtime host in main loop. */
    void           ( *tick )( void );

} job_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( JOB_STATIC )
    MOD_GATEWAY_STATIC( job_api_t, job )
#else
    MOD_GATEWAY_DYNAMIC( job_api_t, job )
#endif

#if defined( BUILD_STATIC ) || defined( JOB_STATIC )
    #define MOD_USE_JOB
    #define MOD_FETCH_JOB  true
#else
    #define MOD_USE_JOB    MOD_DEFINE_API_PTR( job_api_t, job )
    #define MOD_FETCH_JOB  MOD_FETCH_API( job_api_t, job )
#endif

// clang-format on
/*============================================================================================*/
#endif    // JOB_API_H
