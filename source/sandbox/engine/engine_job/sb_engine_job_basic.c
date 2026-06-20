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
    // Step 1: Initialize ORB's module loader system.
    mod_system_init();

    // Step 2: Register static engine modules ('sys' and 'job').
    if ( !mod_static( sys ) || !mod_static( job ) )
    {
        fprintf( stderr, "Failed to load baseline engine modules: %s\n", mod_last_error() );
        goto shutdown;
    }

    // Step 3: Initialize all loaded modules (runs sys_mod_init and job_mod_init).
    if ( !mod_init_all() )
    {
        fprintf( stderr, "Failed to initialize modules: %s\n", mod_last_error() );
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
    job_counter_t counter = job()->dispatch( jobs, 3 );

    printf( "Waiting for jobs to finish...\n" );
    // Step 6: Block until the counter hits zero.
    job()->wait( counter );

    printf( "Ticking job system...\n" );
    job()->tick();

    printf( "Basic job system tests passed.\n" );

shutdown:
    // Step 7: Clean up all modules.
    mod_system_exit();
}
