/*==============================================================================================

    render.c -- Unity build entry for the render module.

    Sits on top of the RHI. Consumes rhi() for all GPU calls; exposes a
    simple begin/draw/end frame surface to the host. State (clear color, frame
    counter, in-flight command list) persists across reloads.

    Layering
    --------
        rhi (static service)     <- Vulkan backend, swap-chain aware
            ^
            | rhi()->...
            |
        render (this DLL)        <- high-level framing, hot-reloadable
            ^
            | render()->...
            |
        host_main on_update      <- calls begin_frame / draw_frame / end_frame

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#define LOG_CH "render"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"
#include "runtime_service/rhi/rhi_api.h"
#include "runtime_modules/render/render_api.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "runtime_modules/render/render_function.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef RENDER_API_C_PRELUDE
#include "runtime_modules/render/render_api.c"
#endif

/*============================================================================================*/
