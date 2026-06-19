/*==============================================================================================

    engine/jobs/jobs_host.h — Host-only jobs API: module descriptor for static registration.
    Includes jobs.h.

==============================================================================================*/
#ifndef JOBS_HOST_H
#define JOBS_HOST_H

#include "engine/jobs/jobs_api.h"
#include "engine/mod/mod_host.h"

/*==============================================================================================

    Module Descriptor

    Used by the host to register the jobs module:
        mod_static( jobs );

==============================================================================================*/

mod_desc_t* jobs_get_mod_desc( void );

/*============================================================================================*/
#endif    // JOBS_HOST_H
