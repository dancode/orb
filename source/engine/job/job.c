/*==============================================================================================

    engine/job/job.c — Unity build entry point for the job module.

==============================================================================================*/
#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/job/job_host.h"

static job_counter_t g_dummy_counter = { 0 };

static job_counter_t*
job_dispatch( const job_decl_t* decls, uint32_t count )
{
    for ( uint32_t i = 0; i < count; ++i )
    {
        if ( decls[ i ].function )
        {
            decls[ i ].function( decls[ i ].data );
        }
    }
    return &g_dummy_counter;
}

static void
job_wait( job_counter_t* counter )
{
    UNUSED( counter );
}

static void
job_tick( void )
{
}

#ifndef JOB_API_C_PRELUDE
#include "engine/job/job_api.c"
#endif
