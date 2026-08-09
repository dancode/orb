#ifndef RENDER_H
#define RENDER_H
/*==============================================================================================

    runtime_modules/render/render.h -- Render module types.

==============================================================================================*/

#include "orb.h"
#include "runtime_service/rhi/rhi.h"

/*==============================================================================================
    Offscreen render targets

    Target ids share the i32 render_ctx space with swapchain context ids (run_view_t hands
    either to a project) but live in a disjoint range so submit_rect/draw calls can route:
    ctx ids are [0..RHI_CTX_MAX), target ids are [BASE..BASE+MAX), -1 stays headless.
==============================================================================================*/

#define RENDER_TARGET_ID_BASE 0x1000
#define RENDER_TARGET_MAX     8

/*============================================================================================*/
#endif    // RENDER_H
