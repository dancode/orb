/*==============================================================================================

    runtime_service/gui/gui_flow.c -- GUI_FLOW translation unit: layout composition.

    This is the part of the GUI that decides WHERE things go. Every widget, panel, and window
    eventually needs an actual rectangle on screen -- an x, y, width, and height in pixels --
    and this file is the only place that does that math. It takes the abstract spacing rules
    from the style system (how big a gap should be, how much padding, how big a button) and
    turns them into real rectangles other code can draw into. Nothing above this layer (the
    widgets, the window chrome) is allowed to compute a rect itself; they ask flow for one.

    The pieces in this file build on each other, roughly simplest to most specialized:

    - A grid engine that splits a row into columns and hands out one cell at a time, the base
      every other layout shape is built from.

    - Scrollable regions: a rectangle with a scrollbar and a clipped viewport, used any time
      content can be taller than the space available for it.

    - Child panels: a boxed sub-area nested inside whatever layout is currently open, optionally
      draggable to resize.

    - Sub-layouts: a short-lived layout dropped over a rectangle the caller already has, for
      laying out a handful of widgets inside something like a table cell.

    - Split panels: two panes side by side (or stacked) with a draggable divider between them,
      remembering how the user last sized them.

    - The root-level region: the simplest of all, just a caller-given rectangle with none of a
      window's chrome, for HUD-style elements that draw and take input without a title bar.

    - The public layout functions and sizing helpers that other code actually calls to make use
      of all of the above.

    - A table engine: columns that can be dragged wider/narrower, click-to-sort headers, and a
      trick called virtualization so a table with thousands of rows only builds the handful
      that are actually visible on screen.

    Flow sits in the middle of the GUI's stack. Below it, flow leans on core (the layout stack
    itself, saved per-widget state, animation easing, input), style (the spacing numbers), and
    interact (the drag-to-resize logic windows and child panels share). Flow also owns the
    on-screen clipping rectangle: it pushes and pops the clip whenever a scroll region or child
    panel opens and closes, so drawing outside a panel's bounds gets cut off automatically. That,
    plus an optional debug outline around each region, is the only drawing flow does itself;
    actual painting -- fill colors, borders, what a scrollbar looks like -- is handed off to
    painter functions supplied by the layer above (documented in flow/gui_flow.h).

    Include order matters below: each file can use functions and variables defined in the files
    listed above it.

    flow/gui_layout_core.c   -- track resolver, cell emitters, the layout-frame resets
    flow/gui_scroll.c        -- scroll region push/pop: view, gutters, scissor, bars
    flow/gui_layout_child.c  -- child_begin/end: the boxed sub-region + edge resize
    flow/gui_sublayout.c     -- transient sub-layouts over a caller rect
    flow/gui_split.c         -- split panels: persisted pair heights
    flow/gui_region.c        -- the root-level region (window-free composition)
    flow/gui_layout.c        -- public layout verbs + the sz_ sizing family
    flow/gui_table_engine.c  -- widget-agnostic table machinery: column tracks + pair-resize,
                                the sort state machine, fixed-pitch row virtualization

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"

/* header files */

#include "runtime_service/gui/render/gui_render.h"
#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/debug/gui_debug.h"

/* unity files */

#include "runtime_service/gui/flow/gui_layout_core.c"
#include "runtime_service/gui/flow/gui_scroll.c"
#include "runtime_service/gui/flow/gui_layout_child.c"
#include "runtime_service/gui/flow/gui_sublayout.c"
#include "runtime_service/gui/flow/gui_split.c"
#include "runtime_service/gui/flow/gui_region.c"
#include "runtime_service/gui/flow/gui_layout.c"
#include "runtime_service/gui/flow/gui_table_engine.c"

/*==============================================================================================
    Memory accounting -- this unit's fixed statics, read by gui_ui_memory (gui_ui_mem.c).
==============================================================================================*/

u32
flow_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_layout_stack ) + sizeof( s_layout_state_stack )
                + sizeof( s_split_stack ) + sizeof( s_sublayout_sink )
                + sizeof( s_table_fit_press ) );
}

/*============================================================================================*/
