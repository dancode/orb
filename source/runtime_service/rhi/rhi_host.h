/*==============================================================================================

    runtime_service/rhi/rhi_host.h -- Host-only RHI interface.  Includes rhi_api.h.

    Declares direct-call functions only accessible to host executables and sandboxes.
    DLL modules include rhi_api.h and call through the vtable only.

==============================================================================================*/
#ifndef RHI_HOST_H
#define RHI_HOST_H

#include "runtime_service/rhi/rhi_api.h"
#include "engine/mod/mod_export.h"

/* Module descriptor -- pass to mod_static_load() to register the RHI with the mod system. */
mod_desc_t* rhi_get_mod_desc( void );

/*============================================================================================*/
#endif    // RHI_HOST_H
