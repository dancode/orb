/*==============================================================================================

    runtime_service/gui/gui_debug.c -- GUI_DEBUG translation unit: server introspection.

    Developer tools for looking INSIDE the GUI while it runs, rather than tools the GUI needs
    in order to function: a pipeline dashboard that shows what the renderer is doing frame to
    frame, and a command stepper that lets a developer freeze a frame and step through its
    draw commands one at a time. Both are just ordinary debug windows, drawn through the same
    public API any other window uses, over a snapshot the render unit captured for them.

    Because nothing else in the GUI depends on this unit, a shipping build could drop it
    entirely and lose nothing but these diagnostics -- that is the whole point of keeping it
    separate.

    Note: the frame-timing overlay (frame/gui_frame_overlay.c, the FPS/perf HUD) is NOT part of
    this unit even though it looks similar. Its numbers are read by the frame lifecycle itself
    to pace the engine, so it counts as core plumbing rather than optional tooling, and lives
    with the frame orchestrator instead.

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
debug_unit_mem_bytes( void )
{
    u32 b = 0;
#ifdef GUI_PIPELINE_DASHBOARD
    b += (u32)sizeof( s_dash_palette );
#endif
    return b;
}

/*============================================================================================*/
