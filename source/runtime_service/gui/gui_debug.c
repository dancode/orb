/*==============================================================================================

    runtime_service/gui/gui_debug.c -- GUI_DEBUG translation unit: server introspection.

    Dev tooling OVER the system, not part of it (severable -- a ship build could drop this
    unit and lose nothing but the diagnostics): the pipeline dashboard and the command
    stepper, each an ordinary debug-band window painted through the standard draw API over a
    snapshot the backend unit captured (render/gui_dash_capture.c / gui_step_capture.c).

    Root unit .c since R10, like every unit (GUI_SERVER_PLAN.md); implementation in debug/,
    cross-unit decls in debug/gui_debug.h.  The compiler enforces that the debug tier reaches
    the rest of gui only through the public gui_* surface (gui_host.h), the backend capture
    API (gui_render.h), and the umbrella's cross-unit seams.

    gui_frame_overlay.c is NOT here ON PURPOSE (frame/gui_frame_overlay.c since R10): it
    carries the frame-timing helpers the frame lifecycle itself calls (gui_frame.c) --
    conductor code, not severable tooling.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "base/fmt.h"

#include "runtime_service/gui/gui_internal.h"   /* -> gui_host.h -> gui_api.h -> gui.h */
#include "runtime_service/gui/render/gui_render.h"    /* capture snapshots + draw_* backend API  */

/* Pipeline dashboard -- debug-band window + panel painters over the dash capture. */
#include "runtime_service/gui/debug/gui_dashboard.c"

/* Command stepper -- debug-band window controlling the frozen-frame replay. */
#include "runtime_service/gui/debug/gui_step_window.c"

/* MEMORY ACCOUNTING: this unit's fixed statics, reported to gui_ui_memory (gui_ui_mem.c)
   through the gui_internal.h seam -- each carved unit accounts for itself.  Last include-order
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
