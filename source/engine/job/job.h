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
   job_counter_t — An opaque synchronization handle returned by dispatch.
   Encodes both a pool slot index and a generation number so stale handles can be detected.
   Pass this by value to job()->wait(). JOB_COUNTER_NULL indicates no active batch.
*/
typedef struct job_counter_s
{
    u32 id; // Packed handle: bits[7:0]=pool index, bits[23:8]=generation (0=null).

} job_counter_t;

// Null/invalid counter handle; returned by dispatch when count==0.
#define JOB_COUNTER_NULL ( (job_counter_t){ 0 } )

/*============================================================================================*/
#endif    // JOB_H
