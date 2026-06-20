/*==============================================================================================

    engine/job/job.h — Job/task system type declarations.

==============================================================================================*/
#ifndef JOB_H
#define JOB_H

#include "orb.h"

/* 
   job_fn_t — The signature for a task function.
   Any function you want the job system to run in parallel must match this signature:
   accepting a single 'void*' pointer parameter (holding user data) and returning nothing.
*/
typedef void ( *job_fn_t )( void* arg );

/* 
   job_decl_t — A single job declaration / work packet.
   This struct packages a pointer to the function to execute along with the custom data
   payload it needs to run. It represents the task definition itself before execution.
*/
typedef struct job_decl_s
{
    job_fn_t function; // Pointer to the function that will execute the job.
    void*    data;     // Pointer to custom parameter data passed to the function.

} job_decl_t;

/* 
   job_counter_t — A synchronization handle used to track active jobs.
   When you dispatch a group of jobs, the system increments this counter's internal value.
   As background worker threads finish running individual jobs, they atomically decrement
   this value. You can block your calling thread and wait for this value to hit zero.
*/
typedef struct job_counter_s
{
    volatile i32 value; // The atomic/volatile count of active tasks remaining in this group.

} job_counter_t;

/*============================================================================================*/
#endif    // JOB_H
