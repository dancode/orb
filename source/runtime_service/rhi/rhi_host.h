/*==============================================================================================

    runtime_service/rhi/rhi_host.h -- Host-only RHI interface.  Includes rhi_api.h.

    Declares direct-call functions that are only accessible to host executables.
    DLL modules include rhi_api.h and call through the vtable only -- they never
    see declarations here.

==============================================================================================*/
#ifndef RHI_HOST_H
#define RHI_HOST_H

#include "runtime_service/rhi/rhi_api.h"

/* No host-only direct calls yet -- add them here as the RHI grows. */

/*============================================================================================*/
#endif    // RHI_HOST_H
