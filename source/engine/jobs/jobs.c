/*==============================================================================================

    engine/jobs/jobs.c — Unity build entry point for the jobs module.

==============================================================================================*/
#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/jobs/jobs_host.h"

static job_counter_t g_dummy_counter = { 0 };

static job_counter_t*
jobs_dispatch( const job_decl_t* decls, uint32_t count )
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
jobs_wait( job_counter_t* counter )
{
    UNUSED( counter );
}

static void
jobs_tick( void )
{
}

#ifndef JOBS_API_C_PRELUDE
#include "engine/jobs/jobs_api.c"
#endif
