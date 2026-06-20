/*==============================================================================================

    engine/job/job.c — Unity build entry point for the job module.

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/sys/sys_host.h"
#include "engine/job/job_host.h"

#define MAX_JOBS_LIMIT 4096

typedef struct worker_thread_s
{
    thread_t    handle;
    uint32_t    index;
    thread_id_t id;

} worker_thread_t;

typedef struct job_item_s
{
    job_fn_t        function;
    void*           data;
    job_counter_t*  counter;

} job_item_t;

typedef struct job_state_s
{
    uint32_t        worker_count;
    worker_thread_t workers[ 32 ];

    job_item_t      queue[ MAX_JOBS_LIMIT ];
    i32             queue_head;
    i32             queue_tail;
    i32             queue_count;

    mutex_t         queue_lock;
    sema_t          queue_semaphore;

    job_counter_t   counter_pool[ 256 ];
    volatile i32    counter_pool_index;

    volatile i32    is_running;

} job_state_t;

static job_state_t* g_job_state = NULL;

static void
job_worker_main( void* arg )
{
    worker_thread_t* self = ( worker_thread_t* )arg;
    self->id = thread_current_id();

    while ( sys_atomic_read( &g_job_state->is_running ) )
    {
        sema_wait( &g_job_state->queue_semaphore );

        if ( !sys_atomic_read( &g_job_state->is_running ) )
        {
            break;
        }

        job_item_t job = { 0 };
        bool       has_job = false;

        mutex_lock( &g_job_state->queue_lock );
        if ( g_job_state->queue_count > 0 )
        {
            i32 index = g_job_state->queue_head % MAX_JOBS_LIMIT;
            job = g_job_state->queue[ index ];
            g_job_state->queue_head++;
            g_job_state->queue_count--;
            has_job = true;
        }
        mutex_unlock( &g_job_state->queue_lock );

        if ( has_job )
        {
            if ( job.function )
            {
                job.function( job.data );
            }
            if ( job.counter )
            {
                sys_atomic_decrement( &job.counter->value );
            }
        }
    }
}

static job_counter_t*
job_allocate_counter( i32 initial_value )
{
    for ( uint32_t i = 0; i < 256; ++i )
    {
        i32            index = sys_atomic_increment( &g_job_state->counter_pool_index ) % 256;
        job_counter_t* counter = &g_job_state->counter_pool[ index ];
        if ( sys_atomic_compare_exchange( &counter->value, initial_value, 0 ) == 0 )
        {
            return counter;
        }
    }
    ORB_PANIC();
    return NULL;
}

static job_counter_t*
job_dispatch( const job_decl_t* decls, uint32_t count )
{
    if ( count == 0 )
    {
        return NULL;
    }

    job_counter_t* counter = job_allocate_counter( ( i32 )count );

    mutex_lock( &g_job_state->queue_lock );
    if ( g_job_state->queue_count + ( i32 )count > MAX_JOBS_LIMIT )
    {
        mutex_unlock( &g_job_state->queue_lock );
        ORB_PANIC();
        return NULL;
    }

    for ( uint32_t i = 0; i < count; ++i )
    {
        i32 index = g_job_state->queue_tail % MAX_JOBS_LIMIT;
        g_job_state->queue[ index ].function = decls[ i ].function;
        g_job_state->queue[ index ].data = decls[ i ].data;
        g_job_state->queue[ index ].counter = counter;

        g_job_state->queue_tail++;
        g_job_state->queue_count++;
    }
    mutex_unlock( &g_job_state->queue_lock );

    sema_post( &g_job_state->queue_semaphore, count );

    return counter;
}

static void
job_wait( job_counter_t* counter )
{
    if ( !counter )
    {
        return;
    }

    while ( sys_atomic_compare_exchange( &counter->value, 0, 0 ) > 0 )
    {
        job_item_t job = { 0 };
        bool       has_job = false;

        mutex_lock( &g_job_state->queue_lock );
        if ( g_job_state->queue_count > 0 )
        {
            i32 index = g_job_state->queue_head % MAX_JOBS_LIMIT;
            job = g_job_state->queue[ index ];
            g_job_state->queue_head++;
            g_job_state->queue_count--;
            has_job = true;
        }
        mutex_unlock( &g_job_state->queue_lock );

        if ( has_job )
        {
            sema_try_wait( &g_job_state->queue_semaphore );

            if ( job.function )
            {
                job.function( job.data );
            }
            if ( job.counter )
            {
                sys_atomic_decrement( &job.counter->value );
            }
        }
        else
        {
            thread_yield();
        }
    }
}

static void
job_tick( void )
{
}

static bool
job_init( void* raw_state )
{
    g_job_state = ( job_state_t* )raw_state;

    g_job_state->queue_head = 0;
    g_job_state->queue_tail = 0;
    g_job_state->queue_count = 0;
    g_job_state->counter_pool_index = 0;
    memset( ( void* )g_job_state->counter_pool, 0, sizeof( g_job_state->counter_pool ) );

    mutex_init( &g_job_state->queue_lock );
    sema_init( &g_job_state->queue_semaphore, 0 );

    g_job_state->is_running = 1;

    uint32_t cpu_count = sys_cpu_count();
    uint32_t worker_count = cpu_count > 1 ? cpu_count - 1 : 1;
    if ( worker_count > 32 )
    {
        worker_count = 32;
    }

    g_job_state->worker_count = worker_count;

    for ( uint32_t i = 0; i < worker_count; ++i )
    {
        worker_thread_t* w = &g_job_state->workers[ i ];
        w->index = i;
        w->handle = thread_create( job_worker_main, w, 0 );
        if ( thread_valid( w->handle ) )
        {
            char name_buf[ 32 ];
            sprintf( name_buf, "ORB_Worker_%02u", i );
            thread_set_name( w->handle, name_buf );
            w->id = 0;
        }
        else
        {
            return false;
        }
    }

    return true;
}

static void
job_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( !g_job_state )
    {
        return;
    }

    sys_atomic_write( &g_job_state->is_running, 0 );

    sema_post( &g_job_state->queue_semaphore, g_job_state->worker_count );

    for ( uint32_t i = 0; i < g_job_state->worker_count; ++i )
    {
        worker_thread_t* w = &g_job_state->workers[ i ];
        if ( thread_valid( w->handle ) )
        {
            thread_join( w->handle );
        }
    }

    mutex_destroy( &g_job_state->queue_lock );
    sema_destroy( &g_job_state->queue_semaphore );

    g_job_state = NULL;
}

#ifndef JOB_API_C_PRELUDE
#define JOB_API_C_PRELUDE
#endif
#include "engine/job/job_api.c"
