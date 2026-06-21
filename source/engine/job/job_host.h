/*==============================================================================================

    engine/job/job_host.h — Host-only job API: module descriptor for static registration.
    Includes job.h.

==============================================================================================*/
#ifndef JOB_HOST_H
#define JOB_HOST_H

#include "engine/job/job_api.h"
#include "engine/mod/mod_host.h"

/*==============================================================================================

    Module Descriptor

    Used by the host to register the job module:
        mod_static( job );

==============================================================================================*/

mod_desc_t*     job_get_mod_desc    ( void );

/*==============================================================================================

    Host API

==============================================================================================*/

bool            job_init            ( void );
void            job_exit            ( void );
job_counter_t   job_dispatch        ( const job_decl_t* decls, uint32_t count );
void            job_wait            ( job_counter_t counter );
void            job_tick            ( void );

/*============================================================================================*/
#endif    // JOB_HOST_H
