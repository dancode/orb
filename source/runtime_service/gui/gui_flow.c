/*==============================================================================================

    runtime_service/gui/gui_flow.c -- GUI_FLOW translation unit: layout composition.

    THE RECT PRODUCER (GUI_SERVER_PLAN.md): the only code that turns style spacing metrics
    (line_size / gap / pad / quantum / scales) into rects.  Track resolver + cell emitters,
    scroll regions, children, transient sub-layouts, split panels, the root-level region, and
    the public layout verbs + sz_ sizing family.  Metrics in, rects out; the layers above
    (element, chrome) consume the rects flow carves.

    Downward, flow composes over core (layout-frame stack, keyed state, anim ease, io),
    style (the spacing metrics), interact (resize_apply_edges -- the child edge-resize rides
    the same mechanism windows use), and the render server's clip stack: flow computes THE
    view rect, so it owns the region scissor lifecycle (draw_push/pop_clip_rect) and the
    matching s_scope.clip interaction fence -- plus the GUI_DBG_REGION outline behind the
    debug gate.  No other render call belongs here: flow places, it does not paint.

    The upward seams live in the documented block of flow/gui_flow.h (mirroring core's and
    interact's): scrollbar_widget -- the region gutter's ONE widget -- and the child box
    paint trio (draw_child_bg / draw_child_border / draw_resize_highlight, styled painters
    bound for the element unit at R8).

    Include order matters: each file can reference statics from files included above it.

    flow/gui_layout_core.c   -- track resolver, cell emitters, the layout-frame resets
    flow/gui_scroll.c        -- scroll region push/pop: view, gutters, scissor, bars
    flow/gui_layout_child.c  -- child_begin/end: the boxed sub-region + edge resize
    flow/gui_sublayout.c     -- transient sub-layouts over a caller rect
    flow/gui_split.c         -- split panels: persisted pair heights
    flow/gui_region.c        -- the root-level region (window-free composition)
    flow/gui_layout.c        -- public layout verbs + the sz_ sizing family

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"

#include "runtime_service/gui/gui_internal.h"   /* umbrella: unit headers in stack order    */
#include "runtime_service/gui/render/gui_render.h"    /* clip stack + GUI_DBG_REGION outline */

#include "runtime_service/gui/flow/gui_layout_core.c"
#include "runtime_service/gui/flow/gui_scroll.c"
#include "runtime_service/gui/flow/gui_layout_child.c"
#include "runtime_service/gui/flow/gui_sublayout.c"
#include "runtime_service/gui/flow/gui_split.c"
#include "runtime_service/gui/flow/gui_region.c"
#include "runtime_service/gui/flow/gui_layout.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  The layout-frame stack itself stays a core static -- core/gui_ctx.c owns
    frame turnover -- and is counted by the core unit.
==============================================================================================*/

u32
gui_flow_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_layout_state_stack ) + sizeof( s_split_stack )
                + sizeof( s_sublayout_sink ) );
}

/*============================================================================================*/
