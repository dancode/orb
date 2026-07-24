/*==============================================================================================

    runtime_service/gui/gui_debug.c -- GUI_DEBUG translation unit: server introspection.

    Dev tooling OVER the system, not part of it (severable -- a ship build could drop this
    unit and lose nothing but the diagnostics): the pipeline dashboard and the command
    stepper, each an ordinary debug-band window painted through the standard draw API over a
    snapshot the backend unit captured (render/gui_dash_capture.c / gui_step_capture.c).

    Root unit .c, like every unit; implementation in debug/,
    cross-unit decls in debug/gui_debug.h.  The compiler enforces that the debug tier reaches
    the rest of gui only through the public gui_* surface (gui_host.h), the backend capture
    API (gui_render.h), and the umbrella's cross-unit seams.

    gui_frame_overlay.c is NOT here ON PURPOSE (frame/gui_frame_overlay.c): it
    carries the frame-timing helpers the frame lifecycle itself calls (gui_frame_loop.c) --
    conductor code, not severable tooling.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "base/fmt.h"

/* This unit's world -- everything (the include list IS the dependency graph; debug
   reads both servers' internals, which is exactly why it is severable and last). */
#include "runtime_service/gui/render/gui_render.h"    /* capture snapshots + draw_* backend API
                                                         (pulls gui_host.h + rhi/app APIs)  */
#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/stock/gui_stock_internal.h"
#include "runtime_service/gui/chrome/gui_chrome.h"
#include "runtime_service/gui/debug/gui_debug.h"

/* Pipeline dashboard -- debug-band window + panel painters over the dash capture. */
#include "runtime_service/gui/debug/gui_dashboard.c"

/* Command stepper -- debug-band window controlling the frozen-frame replay. */
#include "runtime_service/gui/debug/gui_step_window.c"

/* MEMORY ACCOUNTING: this unit's fixed statics, reported to gui_ui_memory (gui_ui_mem.c)
   through the debug/gui_debug.h seam -- each carved unit accounts for itself.  Last include-order
   position so every static aggregate above is in scope. */
u32
gui_debug_unit_mem_bytes( void )
{
    u32 b = 0;
#ifdef GUI_PIPELINE_DASHBOARD
    b += (u32)sizeof( s_dash_palette );
#endif
    return b;
}

/*============================================================================================*/
