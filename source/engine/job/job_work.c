/*==============================================================================================

    engine/job/job_work.c — Worker thread implementation for the job system.

==============================================================================================*/
// clang-format off

// Handle encoding: bits[7:0] = pool index (0-255), bits[23:8] = generation (1-65535; 0=null).
#define JOB_HANDLE_INDEX( id )       ( (u32)(id) & 0xFFu )
#define JOB_HANDLE_GEN( id )         ( (u32)(id) >> 8 & 0xFFFFu )
#define JOB_HANDLE_MAKE( idx, gen )  ( (u32)(idx) | ( (u32)(gen) << 8 ) )

/*==============================================================================================
   job_complete_one — Signals one job's completion against its batch counter.

   Decodes the handle, validates the slot generation, and decrements the live count. A
   generation mismatch means the slot was already recycled, so we treat it as a safe no-op.
==============================================================================================*/

static void
job_complete_one( job_counter_t counter )
{
    // Null handle -- this job was not tracked by a counter.
    if ( counter.id == 0 )
    {
        return;
    }

    u32              index = JOB_HANDLE_INDEX( counter.id );
    job_pool_slot_t* slot  = &g_job_state.counter_pool[ index ];

    // Only decrement if the slot still belongs to this batch.
    if ( slot->generation == ( u16 )JOB_HANDLE_GEN( counter.id ) )
    {
        // Decrement the batch counter atomically. When it hits 0 the slot is free.
        sys_atomic_decrement( &slot->value );
    }
}

/*==============================================================================================
   job_worker_main — The entry point and execution loop for every worker thread.

   1. Sleep on the semaphore until there is work available.
   2. Wake up, lock the queue mutex, and pop the next job from the head of the queue.
   3. Unlock the mutex, run the job function, and decrement the associated pool slot.
==============================================================================================*/

static void
job_worker_main( void* arg )
{
    worker_thread_t* self = ( worker_thread_t* )arg;
    self->id              = thread_current_id();    // Cache the thread's OS ID.

    // Keep running as long as the system is active.
    while ( sys_atomic_read( &g_job_state.is_running ) )
    {
        // Block-sleep until the semaphore count is greater than 0.
        // When jobs are dispatched, sema_post raises this count, waking up workers.
        sema_wait( &g_job_state.queue_semaphore );

        // If the system shut down while we were sleeping, exit the thread.
        if ( !sys_atomic_read( &g_job_state.is_running ) )
        {
            break;
        }

        job_item_t job     = { 0 };
        bool       has_job = false;

        // Pop an item from the queue ring buffer. Must lock to avoid race conditions
        // with other worker threads popping, or external threads pushing.
        mutex_lock( &g_job_state.queue_lock );
        if ( g_job_state.queue_count > 0 )
        {
            // Calculate the ring buffer index using modulo.
            i32 index = g_job_state.queue_head % MAX_JOBS_LIMIT;
            job       = g_job_state.queue[ index ];

            // Advance the monotonic head index.
            g_job_state.queue_head++;
            g_job_state.queue_count--;
            has_job = true;
        }
        mutex_unlock( &g_job_state.queue_lock );

        // If we successfully popped a job, execute it.
        if ( has_job )
        {
            if ( job.function )
            {
                // Run the job callback function with its argument data.
                job.function( job.data );
            }
            // Signal completion against this job's batch counter.
            job_complete_one( job.counter );
        }
    }
}

/*==============================================================================================
   job_allocate_counter — Claims a pool slot and returns an opaque counter handle.

   Loops the circular pool using CAS to find a free slot (value==0), then bumps its
   generation and returns a handle encoding (index, generation). Generation 0 is reserved
   for the null handle, so after a u16 wrap the allocator skips back to 1.
==============================================================================================*/

static job_counter_t
job_allocate_counter( i32 initial_value )
{
    for ( uint32_t i = 0; i < 256; ++i )
    {
        // Get the next index in the pool atomically.
        i32              index = sys_atomic_increment( &g_job_state.counter_pool_index ) % 256;
        job_pool_slot_t* slot  = &g_job_state.counter_pool[ index ];

        // CAS: if slot->value is 0, claim it by setting initial_value.
        // Only one thread will succeed; others continue scanning.
        if ( sys_atomic_compare_exchange( &slot->value, initial_value, 0 ) == 0 )
        {
            // Bump generation; skip 0 so null handle (id==0) stays distinct.
            u16 gen = ++slot->generation;
            if ( gen == 0 )
            {
                gen = ++slot->generation;
            }
            return (job_counter_t){ .id = JOB_HANDLE_MAKE( index, gen ) };
        }
    }
    // Panic if all 256 slots are currently in use (pool exhausted).
    ORB_PANIC();
    return JOB_COUNTER_NULL;
}

/*==============================================================================================
   job_dispatch — Enqueues a batch of parallel jobs and returns an awaitable counter handle.

   1. Allocates a pool slot initialized to 'count'.
   2. Locks the queue, copies declarations into the ring buffer with the slot pointer.
   3. Unlocks the queue and wakes up workers by posting 'count' times to the semaphore.
==============================================================================================*/
job_counter_t
job_dispatch( const job_decl_t* decls, uint32_t count )
{
    if ( count == 0 )
    {
        return JOB_COUNTER_NULL;
    }

    // Allocate a pool slot tracking this batch.
    job_counter_t    handle = job_allocate_counter( ( i32 )count );

    mutex_lock( &g_job_state.queue_lock );
    // Ensure the queue does not overflow.
    if ( g_job_state.queue_count + ( i32 )count > MAX_JOBS_LIMIT )
    {
        mutex_unlock( &g_job_state.queue_lock );
        ORB_PANIC();
        return JOB_COUNTER_NULL;
    }

    // Push the jobs into the ring buffer.
    for ( uint32_t i = 0; i < count; ++i )
    {
        i32 index                           = g_job_state.queue_tail % MAX_JOBS_LIMIT;
        g_job_state.queue[ index ].function = decls[ i ].function;
        g_job_state.queue[ index ].data     = decls[ i ].data;
        g_job_state.queue[ index ].counter  = handle;

        g_job_state.queue_tail++;
        g_job_state.queue_count++;
    }
    mutex_unlock( &g_job_state.queue_lock );

    // Signal the worker threads. Wakes up to 'count' sleeping threads.
    sema_post( &g_job_state.queue_semaphore, count );

    return handle;
}

/*==============================================================================================   
   job_wait — Blocks the caller until the specified batch counter hits zero.

   Generation validation: if the handle's generation does not match the current slot
   generation, the counter was already recycled and we return immediately.

   Wait-Stealing (Active waiting):
   Instead of block-sleeping the thread (which would waste CPU cycles), the thread entering
   this wait helps execute jobs from the queue.

   - If there are jobs in the queue, it pops and runs one.
   - If the queue is empty, it yields its CPU time slice so other workers can run.
==============================================================================================*/

void
job_wait( job_counter_t counter )
{
    // Null handle -- nothing to wait on.
    if ( counter.id == 0 )
    {
        return;
    }

    u32              index = JOB_HANDLE_INDEX( counter.id );
    u16              gen   = ( u16 )JOB_HANDLE_GEN( counter.id );
    job_pool_slot_t* slot  = &g_job_state.counter_pool[ index ];

    // If the generation doesn't match, the slot was already recycled -- treat as done.
    if ( slot->generation != gen )
    {
        return;
    }

    // Spin-wait while the slot still belongs to our batch and has live jobs.
    // Re-checking the generation each iteration guards against the slot being reclaimed
    // by an unrelated batch the moment ours hits 0 -- without it we could end up waiting
    // on a different batch's counter.
    while ( slot->generation == gen && sys_atomic_compare_exchange( &slot->value, 0, 0 ) > 0 )
    {
        job_item_t job     = { 0 };
        bool       has_job = false;

        // Try to pop a job to execute ourselves.
        mutex_lock( &g_job_state.queue_lock );
        if ( g_job_state.queue_count > 0 )
        {
            i32 idx = g_job_state.queue_head % MAX_JOBS_LIMIT;
            job     = g_job_state.queue[ idx ];
            g_job_state.queue_head++;
            g_job_state.queue_count--;
            has_job = true;
        }
        mutex_unlock( &g_job_state.queue_lock );

        if ( has_job )
        {
            // Since we popped a job directly without using sema_wait(), the semaphore
            // count is now off by 1. We consume a semaphore token to correct it.
            sema_try_wait( &g_job_state.queue_semaphore );

            if ( job.function )
            {
                // Run the job payload inline.
                job.function( job.data );
            }
            // Signal completion against this job's batch counter.
            job_complete_one( job.counter );
        }
        else
        {
            // If the queue is empty, yield the CPU thread slice to prevent hot-loop spinning
            // and let background threads finish their tasks.
            thread_yield();
        }
    }
}

/*==============================================================================================
   job_tick — Per-frame update hook. Unused in Tier I.
==============================================================================================*/

void
job_tick( void )
{
    // empty
}

/*==============================================================================================
   job_init — Sets up the thread pool and queue system.
   Invoked by the module lifecycle initialization callback.
==============================================================================================*/

bool
job_init( void )
{
    // Reset indices.
    g_job_state.queue_head         = 0;
    g_job_state.queue_tail         = 0;
    g_job_state.queue_count        = 0;
    g_job_state.counter_pool_index = 0;
    memset( ( void* )g_job_state.counter_pool, 0, sizeof( g_job_state.counter_pool ) );

    // Initialize OS threading primitives.
    mutex_init( &g_job_state.queue_lock );
    sema_init( &g_job_state.queue_semaphore, 0 );

    g_job_state.is_running = 1;

    // Determine the ideal number of threads.
    // Standard practice: Logical core count minus 1 (the main thread acts as the coordinator).
    uint32_t cpu_count    = sys_cpu_count();
    uint32_t worker_count = cpu_count > 1 ? cpu_count - 1 : 1;
    if ( worker_count > 32 )
    {
        worker_count = 32;    // Limit pool size to 32 worker threads.
    }

    g_job_state.worker_count = worker_count;

    // Spawn the worker threads.
    for ( uint32_t i = 0; i < worker_count; ++i )
    {
        worker_thread_t* w = &g_job_state.workers[ i ];
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

/*==============================================================================================
   job_exit — Gracefully shuts down the thread pool and releases OS resources.
   Invoked by the module lifecycle exit callback.
==============================================================================================*/

void
job_exit( void )
{
    // Set is_running to 0 so workers exit their loop.
    sys_atomic_write( &g_job_state.is_running, 0 );

    // Post to the semaphore worker_count times to wake up all sleeping workers
    // so they can see is_running == 0 and exit cleanly.
    sema_post( &g_job_state.queue_semaphore, g_job_state.worker_count );

    // Block and wait for all workers to shut down.
    for ( uint32_t i = 0; i < g_job_state.worker_count; ++i )
    {
        worker_thread_t* w = &g_job_state.workers[ i ];
        if ( thread_valid( w->handle ) )
        {
            thread_join( w->handle );    // Join blocking call.
        }
    }

    // Destroy OS primitives to prevent resource leaks.
    mutex_destroy( &g_job_state.queue_lock );
    sema_destroy( &g_job_state.queue_semaphore );
}

/*============================================================================================*/