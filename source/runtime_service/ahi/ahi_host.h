/*==============================================================================================

    runtime_service/ahi/ahi_host.h -- Host-only AHI interface.  Includes ahi_api.h.

    Include this in host executables, unity build entries, and test sandboxes.
    DLL modules that only drive audio through the vtable include ahi_api.h.

    How to register it in a sandbox or host:

        #include "runtime_service/ahi/ahi_host.h"

        mod_static( ahi );    // or: mod_static_load( "ahi", ahi_get_mod_desc() )

    The dep "core" in the mod_desc_t ensures logging is up first.  The service starts the
    device and audio thread in init() and tears them down in exit(); if no output device
    exists it stays loaded in silent mode (sample_rate() == 0, play() returns invalid).

==============================================================================================*/
#ifndef AHI_HOST_H
#define AHI_HOST_H

#include "runtime_service/ahi/ahi_api.h"
#include "engine/mod/mod_export.h"

/* Module descriptor -- pass to mod_static_load() to register the ahi service. */
mod_desc_t* ahi_get_mod_desc( void );

/*============================================================================================*/
#endif    // AHI_HOST_H
