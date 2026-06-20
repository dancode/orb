/*==============================================================================================

    sandbox/engine/engine_job/sb_engine_job.c — Test job module.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/job/job_host.h"

static void
test_job_fn( void* arg )
{
    const char* message = ( const char* )arg;
    printf( "Job Executed: '%s' on Thread ID: %llu\n", message, ( unsigned long long )thread_current_id() );
}

void
job_test( void )
{
    mod_system_init();

    if ( !mod_static_load( "sys", sys_get_mod_desc() ) ||
         !mod_static_load( "job", job_get_mod_desc() ) )
    {
        fprintf( stderr, "Failed to load baseline engine modules: %s\n", mod_last_error() );
        goto shutdown;
    }

    if ( !mod_init_all() )
    {
        fprintf( stderr, "Failed to initialize modules: %s\n", mod_last_error() );
        goto shutdown;
    }

    printf( "Main Thread ID: %llu\n", ( unsigned long long )thread_current_id() );
    printf( "Job system loaded successfully.\n" );

    /* Test dispatching jobs */
    job_decl_t jobs[ 3 ] = {
        { test_job_fn, "Hello from Job 1" },
        { test_job_fn, "Hello from Job 2" },
        { test_job_fn, "Hello from Job 3" },
    };

    printf( "Dispatching jobs...\n" );
    job_counter_t* counter = job()->dispatch( jobs, 3 );

    printf( "Waiting for jobs to finish...\n" );
    job()->wait( counter );

    printf( "Ticking job system...\n" );
    job()->tick();

    printf( "Job system tests passed.\n" );

shutdown:
    mod_system_exit();
}

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    job_test();
    return 0;
}
