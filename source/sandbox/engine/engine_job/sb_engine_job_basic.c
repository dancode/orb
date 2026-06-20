/*==============================================================================================

    sb_engine_job_basic.c — Basic usage of the job module.

==============================================================================================*/

/*==============================================================================================
    test_job_fn — The callback executed by the job system.
    Prints the custom message argument along with the OS thread ID it executes on.
==============================================================================================*/

static void
test_job_fn( void* arg )
{
    const char* message = ( const char* )arg;
    
    // Prints the string argument and the ID of the thread executing this function.
    // Notice that thread ID is different for different tasks!
    printf( "Job Executed: '%s' on Thread ID: %llu\n", message, ( unsigned long long )thread_current_id() );
}

/*==============================================================================================
   job_test_basic — Main sandbox orchestration routine.
==============================================================================================*/

static void
job_test_basic( void )
{
    // Initialize sys manually.
    sys_tick_init();

    // Initialize job system locally.
    if ( !job_init() )
    {
        fprintf( stderr, "Failed to initialize job system locally.\n" );
        goto shutdown;
    }

    // Print the main thread ID so we can verify if tasks execute on workers or the main thread.
    printf( "Main Thread ID: %llu\n", ( unsigned long long )thread_current_id() );
    printf( "Job system loaded successfully.\n" );

    // Step 4: Define tasks to run in parallel.
    job_decl_t jobs[ 3 ] = {
        { test_job_fn, "Hello from Job 1" },
        { test_job_fn, "Hello from Job 2" },
        { test_job_fn, "Hello from Job 3" },
    };

    printf( "Dispatching jobs...\n" );
    // Step 5: Dispatch the batch of tasks to the system.
    job_counter_t counter = job_dispatch( jobs, 3 );

    printf( "Waiting for jobs to finish...\n" );
    // Step 6: Block until the counter hits zero.
    job_wait( counter );

    printf( "Ticking job system...\n" );
    job_tick();

    printf( "Basic job system tests passed.\n" );

shutdown:
    job_exit();
    sys_tick_exit();
}
