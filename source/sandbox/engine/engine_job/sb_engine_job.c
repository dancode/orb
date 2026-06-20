/*==============================================================================================

    sandbox/engine/engine_job/sb_engine_job.c — Test sandbox for the job module.

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/job/job_host.h"

// Unity includes for the separated test suites
#include "sb_engine_job_basic.c"
#include "sb_engine_job_stress.c"

/*==============================================================================================
   main — Executable entry point.
==============================================================================================*/

int
main( int argc, char** argv )
{
    // UNUSED macros prevent compiler warnings about unused parameters.
    UNUSED( argc );
    UNUSED( argv );
    
    printf( "========================================\n" );
    printf( " Running basic job system test...\n" );
    printf( "========================================\n" );
    job_test_basic();
    
    printf( "\n========================================\n" );
    printf( " Running stress job system test...\n" );
    printf( "========================================\n" );
    job_test_stress();

    return 0;
}

/*============================================================================================*/