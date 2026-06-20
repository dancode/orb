/*==============================================================================================

    sandbox/engine/engine_job/sb_engine_job.c — Test sandbox for the job module.

    GOAL:
    Demonstrate how to load the ORB module system, register static modules,
    dispatch parallel workloads, and synchronize execution.

    EXPLANATION FOR NOVICES:
    1. Module Loading: We register the platform module ('sys') and our job system module ('job')
       with the static module registry, then initialize them.
    2. Parallel Dispatch: We package 3 different task descriptions into a static array. Each one
       points to the same function but carries a different text payload.
    3. Active Wait: We dispatch the tasks to the job pool and retrieve a counter. We call wait(),
       which blocks the main thread until the tasks complete. Because we use "wait-stealing", the
       main thread might help run these tasks instead of idling!
    4. Clean Exit: Once verification passes, we invoke system exit to join worker threads and
       release locks.

==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/job/job_host.h"

/*
   test_job_fn — The callback executed by the job system.
   Prints the custom message argument along with the OS thread ID it executes on.
*/
static void
test_job_fn( void* arg )
{
    const char* message = ( const char* )arg;
    
    // Prints the string argument and the ID of the thread executing this function.
    // Notice that thread ID is different for different tasks!
    printf( "Job Executed: '%s' on Thread ID: %llu\n", message, ( unsigned long long )thread_current_id() );
}

/*
   job_test — Main sandbox orchestration routine.
*/
void
job_test( void )
{
    // Step 1: Initialize ORB's module loader system.
    mod_system_init();

    // Step 2: Register static engine modules ('sys' and 'job').
    // Since this is a static monolithic sandbox build, modules are linked directly into the
    // executable. We load them by passing their module descriptors.
    if ( !mod_static_load( "sys", sys_get_mod_desc() ) ||
         !mod_static_load( "job", job_get_mod_desc() ) )
    {
        fprintf( stderr, "Failed to load baseline engine modules: %s\n", mod_last_error() );
        goto shutdown;
    }

    // Step 3: Initialize all loaded modules (runs sys_mod_init and job_mod_init).
    // This will spawn the background worker thread pool.
    if ( !mod_init_all() )
    {
        fprintf( stderr, "Failed to initialize modules: %s\n", mod_last_error() );
        goto shutdown;
    }

    // Print the main thread ID so we can verify if tasks execute on workers or the main thread.
    printf( "Main Thread ID: %llu\n", ( unsigned long long )thread_current_id() );
    printf( "Job system loaded successfully.\n" );

    // Step 4: Define tasks to run in parallel.
    // We create a static array of three job_decl_t structures.
    // Each structure specifies the execution callback (test_job_fn) and its argument.
    job_decl_t jobs[ 3 ] = {
        { test_job_fn, "Hello from Job 1" },
        { test_job_fn, "Hello from Job 2" },
        { test_job_fn, "Hello from Job 3" },
    };

    printf( "Dispatching jobs...\n" );
    // Step 5: Dispatch the batch of tasks to the system.
    // The system pushes these tasks into the global queue, signals workers, and returns
    // a synchronization counter.
    job_counter_t* counter = job()->dispatch( jobs, 3 );

    printf( "Waiting for jobs to finish...\n" );
    // Step 6: Block until the counter hits zero.
    // If the background worker threads haven't finished the tasks yet, this call will
    // actively execute tasks from the queue on this thread to keep the CPU busy.
    job()->wait( counter );

    printf( "Ticking job system...\n" );
    // Tick the job system to process frame-level cleanup (no-op in Tier I).
    job()->tick();

    printf( "Job system tests passed.\n" );

shutdown:
    // Step 7: Clean up all modules.
    // Calls job_mod_exit to join worker threads and release mutexes, then exits the system.
    mod_system_exit();
}

/*
   main — Executable entry point.
*/
int
main( int argc, char** argv )
{
    // UNUSED macros prevent compiler warnings about unused parameters.
    UNUSED( argc );
    UNUSED( argv );
    
    // Run the sandbox routine.
    job_test();
    return 0;
}