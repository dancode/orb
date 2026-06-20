/*==============================================================================================

    sb_engine_job_stress.c — Stress test for the job module.

==============================================================================================*/

#define STRESS_BATCH_COUNT 100
#define STRESS_JOBS_PER_BATCH 4000

static volatile i32 g_stress_counter = 0;

/*==============================================================================================
    stress_job_fn — The callback executed by the job system for the stress test.
    Increments a global atomic counter.
==============================================================================================*/

static void
stress_job_fn( void* arg )
{
    UNUSED( arg );
    sys_atomic_increment( &g_stress_counter );
}

/*==============================================================================================
   job_test_stress — Stress test orchestration routine.
==============================================================================================*/

static void
job_test_stress( void )
{
    // Initialize sys manually.
    sys_tick_init();

    if ( !job_init() )
    {
        fprintf( stderr, "Failed to initialize job system locally.\n" );
        goto shutdown;
    }

    printf( "Starting stress test: %d batches of %d jobs (Total: %d)...\n", 
            STRESS_BATCH_COUNT, STRESS_JOBS_PER_BATCH, STRESS_BATCH_COUNT * STRESS_JOBS_PER_BATCH );
    
    sys_atomic_write( &g_stress_counter, 0 );

    for ( int b = 0; b < STRESS_BATCH_COUNT; ++b )
    {
        job_decl_t* jobs = ( job_decl_t* )malloc( sizeof( job_decl_t ) * STRESS_JOBS_PER_BATCH );
        if ( !jobs )
        {
            fprintf( stderr, "Failed to allocate memory for jobs!\n" );
            goto shutdown;
        }

        for ( int i = 0; i < STRESS_JOBS_PER_BATCH; ++i )
        {
            jobs[ i ].function = stress_job_fn;
            jobs[ i ].data = NULL;
        }

        job_counter_t counter = job_dispatch( jobs, STRESS_JOBS_PER_BATCH );
        job_wait( counter );
        
        free( jobs );
    }

    i32 expected_count = STRESS_BATCH_COUNT * STRESS_JOBS_PER_BATCH;
    i32 final_count = sys_atomic_read( &g_stress_counter );
    
    if ( final_count == expected_count )
    {
        printf( "Stress test passed! All %d jobs executed correctly.\n", expected_count );
    }
    else
    {
        fprintf( stderr, "Stress test FAILED! Expected %d, got %d.\n", expected_count, final_count );
    }

    job_tick();

shutdown:
    job_exit();
    sys_tick_exit();
}
