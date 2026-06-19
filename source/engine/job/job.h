/*==============================================================================================

    engine/job/job.h — Job/task system type declarations.

==============================================================================================*/
#ifndef JOB_H
#define JOB_H

#include "orb.h"

/*==============================================================================================
    Types & Callbacks
==============================================================================================*/

typedef void ( *job_fn_t )( void* arg );

typedef struct job_decl_s
{
    job_fn_t function;
    void*    data;

} job_decl_t;

/* Handle representing a group of dispatched jobs that can be awaited. */
typedef struct job_counter_s
{
    uint32_t value;

} job_counter_t;

/*============================================================================================*/
#endif    // JOB_H
