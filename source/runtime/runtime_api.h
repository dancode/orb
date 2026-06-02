#ifndef RUNTIME_API_H
#define RUNTIME_API_H
/*==============================================================================================

    runtime/runtime_api.h — runtime module API struct and gateway macro.

==============================================================================================*/

#include "runtime/runtime.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct run_api_s
{
    const run_clock_t* ( *clock )( void );  /* current frame clock (read-only)     */
    void ( *set_time_scale )( f32 );        /* adjust time scale from game or host */

} run_api_t;

#if defined( BUILD_STATIC ) || defined( RUN_STATIC )
    MOD_GATEWAY_STATIC( run_api_t, run )
#else
    MOD_GATEWAY_DYNAMIC( run_api_t, run )
#endif

#if defined( BUILD_STATIC ) || defined( RUN_STATIC )
    #define MOD_USE_RUN    /* static build */
    #define MOD_FETCH_RUN  true
#else
    #define MOD_USE_RUN    MOD_DEFINE_API_PTR( run_api_t, run )
    #define MOD_FETCH_RUN  MOD_FETCH_API( run_api_t, run )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RUNTIME_API_H
