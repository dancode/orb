/*==============================================================================================

    runtime/runtime.c — Unity build entry point for the runtime (host) module.

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

/*==============================================================================================
    Engine headers
==============================================================================================*/

/* static modules always used by the runtime */
#include "engine/sys/sys_host.h"        // system: auto-wired on every DLL load
#include "engine/ref/ref_host.h"        // reflection: auto-wired on every DLL load

/*==============================================================================================
    (Optional) Module API's
==============================================================================================*/

#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "engine/job/job_host.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_modules/render/render_api.h"


/*==============================================================================================
    Our API
==============================================================================================*/

#include "runtime/runtime_api.h"

/*==============================================================================================
    Runtime Headers
==============================================================================================*/

#include "runtime/runtime.h"            // module API (hosts and clients).
#include "runtime/runtime_host.h"       // hosts API (entry point, boot sequence, main loop, etc).

/*==============================================================================================
    Unity Build (The Host Entry Point)
==============================================================================================*/

#ifndef HOST_API_C_PRELUDE
#include "runtime/host/host_api.c"    // Host API definition (exported to modules).
#endif
#include "runtime/host/host_main.c"    // The main() entry point and boot sequence.

/*==============================================================================================
    Unity API Definition
==============================================================================================*/



/*============================================================================================*/