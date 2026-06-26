/*==============================================================================================

    runtime/runtime.c — Unity build entry point for the runtime (host) module.

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

/*==============================================================================================
    Optional-service access mode (must precede every engine/service include below)

    The runtime host is a shared library linked into many host exes, each of which selects a
    different subset of the *host-fetched* services (rhi/draw/gui/render) via its module table.
    Those four are wired by MOD_HOST_FETCH_API() below and guarded with if ( rhi() ) etc.
    Under a monolithic build the normal static gateway would hard-bind these accessors to
    g_<svc>_api_struct, forcing every host -- even a headless server or a CLI/tool window -- to
    link services it never drives, and making the presence guards compile to always-true. Defining
    this here opts THIS translation unit (only) into the pointer gateway even under BUILD_STATIC, so
    those services stay truly optional: present == non-NULL ptr, absent == NULL, exactly as in the
    dynamic build. (rhi is in this set too -- a console or tool host may run without a renderer, so
    the whole GPU bring-up in host_main is gated behind if ( rhi() ).)
    Every other module keeps the direct, devirtualized static gateway. The guard is read when the
    gateway / MOD_USE_* / MOD_FETCH_* / MOD_HOST_FETCH_API macros are defined, so it must be set
    before the first include that pulls mod_import.h / mod_host.h.
==============================================================================================*/

#define MOD_HOST_DYNAMIC_SERVICES

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
#include "runtime_service/draw/draw_api.h"
#include "runtime_service/gui/gui_api.h"
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