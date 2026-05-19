/*==============================================================================================

    runtime/runtime.c — Unity build entry point for the runtime (host) module.

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "engine/mod/mod_export.h" /* for exporting api description */
#include "engine/mod/mod_host.h"   /* module setup and loading (hosts only) */

/* static modules used by runtime */
#include "engine/sys/sys_host.h"
#include "engine/app/app_api.h"
#include "engine/rs/rs_host.h"     /* reflection: auto-wired on every DLL load */

/*==============================================================================================
    (Optional Module Headers
==============================================================================================*/

#include "runtime_service/rhi/rhi.h"
#include "runtime_modules/render/render.h"

/*==============================================================================================
    Runtime Headers
==============================================================================================*/

#include "runtime.h"    // module API (hosts and clients).
#include "host.h"       // hosts API (entry point, boot sequence, main loop, etc).

/*==============================================================================================
    Unity Build
==============================================================================================*/

#include "host/host_main.c"    // The main() entry point and boot sequence.

/*==============================================================================================
    Unity API Definition
==============================================================================================*/

#include "host/host_api.c"    // Host API definition (exported to modules).

/*============================================================================================*/