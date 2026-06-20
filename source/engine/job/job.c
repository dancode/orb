/*==============================================================================================

    engine/job/job.c — Unity build entry point for the job module.

    GOAL:
    Implement a very fast, light-weight, task-based job system for parallelizing workloads
    across all available CPU cores.

    DESIGN (Tier I):
    - Worker Pool: A set of background threads that run in an infinite loop waiting for work.
    - Centralized Queue: A simple ring-buffer containing job descriptors (function pointer + argument).
    - Mutex Guard: A fast mutual-exclusion lock protects the queue's read/write operations.
    - Counting Semaphore: Worker threads sleep when the queue is empty. When jobs are pushed,
      we post to the semaphore to wake up the required number of workers.
    - Wait-Stealing: Instead of blocking when waiting for a job to finish, the calling thread
      actively helps execute pending tasks from the queue to prevent deadlocks and save CPU cycles.

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/sys/sys_host.h"
#include "engine/job/job_host.h"

/*==============================================================================================
    Internal Constants
==============================================================================================*/

// The maximum number of jobs that can be buffered in the queue at any given time.
#define MAX_JOBS_LIMIT 4096

/*==============================================================================================
    Internal Types and State
==============================================================================================*/

/*
   worker_thread_t — Bookkeeping structure for a single worker thread.
   We spawn several of these threads to do the actual parallel processing.
*/
typedef struct worker_thread_s
{
    thread_t        handle;                     // OS handle to the thread.
    thread_id_t     id;                         // OS Thread ID.
    uint32_t        index;                      // Worker index (e.g. 0 to NumWorkers-1).

} worker_thread_t;

/*
   job_pool_slot_t — Internal counter pool entry.
   Holds the live job count plus a generation number. The generation is bumped every time
   this slot is claimed, so callers holding old handles can detect recycled slots.

   The two fields share a single 64-bit word ('packed') so the live count and generation can
   be read as one atomic snapshot -- a waiter can never observe the count of an unrelated batch
   that recycled this slot. Layout (little-endian): bits[31:0]=value, bits[63:32]=generation.
   The named members are for direct/debugger access; concurrent code operates on 'packed'.
*/
typedef union job_pool_slot_u
{
    volatile i64 packed; // Atomic word combining value (low dword) and generation (high dword).
    struct
    {
        volatile i32 value;      // Live job count; 0 means the slot is free.
        volatile u32 generation; // Incremented on each claim; wraps, skipping 0.
    };

} job_pool_slot_t;

/*
   job_item_t — An internal representation of a queued job.
   Carries the batch's counter handle so the worker can signal its completion. The handle
   (index + generation) survives slot recycling and state relocation, unlike a raw pointer.
*/
typedef struct job_item_s
{
    job_fn_t      function; // The task function to run.
    void*         data;     // Argument to pass to the function.
    job_counter_t counter;  // Handle tracking this batch's completion.

} job_item_t;

/*
   job_state_t — The global persistent state for the job system.
   Allocated and managed by ORB's module loader, preserving memory across DLL reloads.
*/
typedef struct job_state_s
{
    // Thread Pool
    uint32_t        worker_count;               // Total active background threads.
    worker_thread_t workers[ 32 ];              // Bookkeeping array for worker threads.

    // Central Queue Ring Buffer
    job_item_t      queue[ MAX_JOBS_LIMIT ];    // Ring buffer storage array.
    i32             queue_head;                 // Monotonic index of the next item to pop.
    i32             queue_tail;                 // Monotonic index of the next slot to push.
    i32             queue_count;                // Current number of items pending in the queue.

    // OS Threading Primitives
    mutex_t         queue_lock;                 // Mutex guarding push/pop modifications on the queue.
    sema_t          queue_semaphore;            // Counting semaphore representing available jobs.

    // Sync Counter Pool
    // 256 slots; each slot tracks one active batch. slot->value==0 means free.
    // Handles returned to callers encode (index, generation) so stale handles are detectable.
    job_pool_slot_t counter_pool[ 256 ];
    volatile i32    counter_pool_index;         // Circular allocator index incremented atomically.

    volatile i32    is_running;                 // Set to 1 when active; set to 0 to trigger worker shutdown.

} job_state_t;

// Global compile-time static instance of the state. Address is resolved at link time via RIP-relative offset.
static job_state_t g_job_state;

/*==============================================================================================
    Unity Includes
==============================================================================================*/

#include "engine/job/job_work.c"

/*==============================================================================================
    API Includes
==============================================================================================*/

#ifndef JOB_API_C_PRELUDE
#define JOB_API_C_PRELUDE
#endif
#include "engine/job/job_api.c"

/*============================================================================================*/