/*==============================================================================================

    runtime/run.c -- Unity build entry point for the runtime (host) module.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
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
#include "engine/prof/prof_host.h"      // profiler: leaf capture kernel, always present
#include "engine/fs/fs_host.h"          // virtual filesystem: leaf on sys, mount + read bytes
#include "engine/job/job_host.h"        // task system: floor -- spawns no threads until configured
#include "engine/net/net_host.h"        // UDP transport: floor -- opens no sockets until peer_create
#include "engine/app/app_host.h"        // windowing/input: floor -- opens no window until window_open
#include "engine/core/core_host.h"      // orchestration: logging, cvars, cmd/console, config

/*==============================================================================================
    (Optional) Service / module API's -- opt-in via k_modules[]
==============================================================================================*/

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_api.h"
#include "runtime_service/gui/gui_api.h"
#include "runtime_service/input/input_api.h"
#include "runtime_modules/render/render_api.h"

/*==============================================================================================
    Our API
==============================================================================================*/

#include "runtime/run_api.h"

/*==============================================================================================
    Runtime Headers
==============================================================================================*/

#include "runtime/run.h"                // module API (hosts and clients).
#include "runtime/run_host.h"           // hosts API (entry point, boot sequence, main loop, etc).

/*==============================================================================================
    Unity Build (constituents)
==============================================================================================*/

#ifndef RUN_API_C_PRELUDE
#include "runtime/run_api.c"     // run module API definition (exported to modules).
#endif

#include "runtime/run_host.c"    // run_host_main: entry point, boot sequence, main loop.
#include "runtime/run_perf.c"    // host-side perf HUD (draw-backed frame-stats overlay).

/*==============================================================================================
    Unity API Definition
==============================================================================================*/



/*============================================================================================*/