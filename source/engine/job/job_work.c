/*==============================================================================================

    engine/job/job_work.c — Worker thread implementation for the job system.

==============================================================================================*/
// clang-format off

/*==============================================================================================
   job_worker_main — The entry point and execution loop for every worker thread.

   1. Sleep on the semaphore until there is work available.
   2. Wake up, lock the queue mutex, and pop the next job from the head of the queue.
   3. Unlock the mutex, run the job function, and decrement the associated counter.
==============================================================================================*/

static void
job_worker_main( void* arg )
{
    worker_thread_t* self = ( worker_thread_t* )arg;
    self->id              = thread_current_id();    // Cache the thread's OS ID.

    // Keep running as long as the system is active.
    while ( sys_atomic_read( &g_job_state->is_running ) )
    {
        // Block-sleep until the semaphore count is greater than 0.
        // When jobs are dispatched, sema_post raises this count, waking up workers.
        sema_wait( &g_job_state->queue_semaphore );

        // If the system shut down while we were sleeping, exit the thread.
        if ( !sys_atomic_read( &g_job_state->is_running ) )
        {
            break;
        }

        job_item_t job     = { 0 };
        bool       has_job = false;

        // Pop an item from the queue ring buffer. Must lock to avoid race conditions
        // with other worker threads popping, or external threads pushing.
        mutex_lock( &g_job_state->queue_lock );
        if ( g_job_state->queue_count > 0 )
        {
            // Calculate the ring buffer index using modulo.
            i32 index = g_job_state->queue_head % MAX_JOBS_LIMIT;
            job       = g_job_state->queue[ index ];

            // Advance the monotonic head index.
            g_job_state->queue_head++;
            g_job_state->queue_count--;
            has_job = true;
        }
        mutex_unlock( &g_job_state->queue_lock );

        // If we successfully popped a job, execute it.
        if ( has_job )
        {
            if ( job.function )
            {
                // Run the job callback function with its argument data.
                job.function( job.data );
            }
            if ( job.counter )
            {
                // Decrement the counter associated with this job group atomically.
                // Once all jobs in the group finish, the counter hits 0.
                sys_atomic_decrement( &job.counter->value );
            }
        }
    }
}

/*
   job_allocate_counter — Claims a job_counter_t from the pool for a new job group.

   Loops through the circular counter pool, using Compare-And-Swap (CAS) to find
   and claim a counter whose current value is 0 (inactive).
*/
static job_counter_t*
job_allocate_counter( i32 initial_value )
{
    for ( uint32_t i = 0; i < 256; ++i )
    {
        // Get the next index in the pool atomically.
        i32            index   = sys_atomic_increment( &g_job_state->counter_pool_index ) % 256;
        job_counter_t* counter = &g_job_state->counter_pool[ index ];

        // CAS: If counter->value is 0, set it to initial_value and return it.
        // This is safe: only one thread will successfully claim this counter.
        if ( sys_atomic_compare_exchange( &counter->value, initial_value, 0 ) == 0 )
        {
            return counter;
        }
    }
    // Panic if all 256 counters are currently in use (pool exhausted).
    ORB_PANIC();
    return NULL;
}

/*
   job_dispatch — Enqueues a batch of parallel jobs and returns an awaitable counter.

   1. Allocates an active sync counter initialized to 'count'.
   2. Locks the queue, copies the declarations into the queue ring buffer, and advances the tail.
   3. Unlocks the queue and wakes up workers by posting 'count' times to the semaphore.
*/
static job_counter_t*
job_dispatch( const job_decl_t* decls, uint32_t count )
{
    if ( count == 0 )
    {
        return NULL;
    }

    // Allocate a counter tracking the completion of these 'count' tasks.
    job_counter_t* counter = job_allocate_counter( ( i32 )count );

    mutex_lock( &g_job_state->queue_lock );
    // Ensure the queue does not overflow.
    if ( g_job_state->queue_count + ( i32 )count > MAX_JOBS_LIMIT )
    {
        mutex_unlock( &g_job_state->queue_lock );
        ORB_PANIC();
        return NULL;
    }

    // Push the jobs into the ring buffer.
    for ( uint32_t i = 0; i < count; ++i )
    {
        i32 index                            = g_job_state->queue_tail % MAX_JOBS_LIMIT;
        g_job_state->queue[ index ].function = decls[ i ].function;
        g_job_state->queue[ index ].data     = decls[ i ].data;
        g_job_state->queue[ index ].counter  = counter;

        g_job_state->queue_tail++;
        g_job_state->queue_count++;
    }
    mutex_unlock( &g_job_state->queue_lock );

    // Signal the worker threads. Wakes up to 'count' sleeping threads.
    sema_post( &g_job_state->queue_semaphore, count );

    return counter;
}

/*
   job_wait — Blocks the caller until the specified job group's counter hits zero.

   Wait-Stealing (Active waiting):
   Instead of block-sleeping the thread (which would waste CPU cycles), the thread entering
   this wait helps execute jobs from the queue.

   - If there are jobs in the queue, it pops and runs one.
   - If the queue is empty, it yields its CPU time slice so other workers can run.
*/
static void
job_wait( job_counter_t* counter )
{
    if ( !counter )
    {
        return;
    }

    // Spin-wait as long as the counter's value is greater than 0.
    while ( sys_atomic_compare_exchange( &counter->value, 0, 0 ) > 0 )
    {
        job_item_t job     = { 0 };
        bool       has_job = false;

        // Try to pop a job to execute ourselves.
        mutex_lock( &g_job_state->queue_lock );
        if ( g_job_state->queue_count > 0 )
        {
            i32 index = g_job_state->queue_head % MAX_JOBS_LIMIT;
            job       = g_job_state->queue[ index ];
            g_job_state->queue_head++;
            g_job_state->queue_count--;
            has_job = true;
        }
        mutex_unlock( &g_job_state->queue_lock );

        if ( has_job )
        {
            // Since we popped a job directly without using sema_wait(), the semaphore
            // count is now off by 1. We consume a semaphore token to correct it.
            sema_try_wait( &g_job_state->queue_semaphore );

            if ( job.function )
            {
                // Run the job payload inline.
                job.function( job.data );
            }
            if ( job.counter )
            {
                // Decrement the counter atomically.
                sys_atomic_decrement( &job.counter->value );
            }
        }
        else
        {
            // If the queue is empty, yield the CPU thread slice to prevent hot-loop spinning
            // and let background threads finish their tasks.
            thread_yield();
        }
    }
}

/*
   job_tick — Per-frame update hook. Unused in Tier I.
*/
static void
job_tick( void )
{}

/*
   job_init — Sets up the thread pool and queue system.
   Invoked by the module lifecycle initialization callback.
*/
static bool
job_init( void* raw_state )
{
    g_job_state = ( job_state_t* )raw_state;

    // Reset indices.
    g_job_state->queue_head         = 0;
    g_job_state->queue_tail         = 0;
    g_job_state->queue_count        = 0;
    g_job_state->counter_pool_index = 0;
    memset( ( void* )g_job_state->counter_pool, 0, sizeof( g_job_state->counter_pool ) );

    // Initialize OS threading primitives.
    mutex_init( &g_job_state->queue_lock );
    sema_init( &g_job_state->queue_semaphore, 0 );

    g_job_state->is_running = 1;

    // Determine the ideal number of threads.
    // Standard practice: Logical core count minus 1 (the main thread acts as the coordinator).
    uint32_t cpu_count    = sys_cpu_count();
    uint32_t worker_count = cpu_count > 1 ? cpu_count - 1 : 1;
    if ( worker_count > 32 )
    {
        worker_count = 32;    // Limit pool size to 32 worker threads.
    }

    g_job_state->worker_count = worker_count;

    // Spawn the worker threads.
    for ( uint32_t i = 0; i < worker_count; ++i )
    {
        worker_thread_t* w = &g_job_state->workers[ i ];
        w->index           = i;

        // Spawn the thread. 0 stack size specifies default stack.
        w->handle = thread_create( job_worker_main, w, 0 );
        if ( thread_valid( w->handle ) )
        {
            char name_buf[ 32 ];
            sprintf( name_buf, "ORB_Worker_%02u", i );
            thread_set_name( w->handle, name_buf );    // Helpful for debugging/profilers.
            w->id = 0;
        }
        else
        {
            return false;    // Thread creation failed, fail module load.
        }
    }

    return true;
}

/*
   job_exit — Gracefully shuts down the thread pool and releases OS resources.
   Invoked by the module lifecycle exit callback.
*/
static void
job_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( !g_job_state )
    {
        return;
    }

    // Set is_running to 0 so workers exit their loop.
    sys_atomic_write( &g_job_state->is_running, 0 );

    // Post to the semaphore worker_count times to wake up all sleeping workers
    // so they can see is_running == 0 and exit cleanly.
    sema_post( &g_job_state->queue_semaphore, g_job_state->worker_count );

    // Block and wait for all workers to shut down.
    for ( uint32_t i = 0; i < g_job_state->worker_count; ++i )
    {
        worker_thread_t* w = &g_job_state->workers[ i ];
        if ( thread_valid( w->handle ) )
        {
            thread_join( w->handle );    // Join blocking call.
        }
    }

    // Destroy OS primitives to prevent resource leaks.
    mutex_destroy( &g_job_state->queue_lock );
    sema_destroy( &g_job_state->queue_semaphore );

    g_job_state = NULL;
}
