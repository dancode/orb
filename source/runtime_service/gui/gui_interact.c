/*==============================================================================================

    runtime_service/gui/gui_interact.c -- GUI_INTERACT translation unit: gesture mechanisms.

    Where the interact server's raw hover/active/focus tracking gets turned into named,
    reusable GESTURES -- the specific things a user actually does with the mouse and keyboard:
    drag something around, resize a panel by its edge, drag-and-drop an item from one place to
    another, type into a text field, extend a multi-item selection with shift-click. Each
    mechanism here takes an id, a rect, and the current input, and hands back a decision --
    "the box moved to here," "a character was typed," "a payload was dropped." None of them
    know what a widget is (a slider, a window), and none of them paint. They are the plumbing a
    widget's logic is built from, one layer above the interact server's raw tracking.

    This unit and the interact server (core/) are the only code allowed to write the shared
    hover/active/focus state directly; everything built on top of this file only claims or
    reads it through documented verbs, never pokes it raw.

    This unit does not draw, with a couple of narrow, deliberate exceptions: the one highlight
    ring shown when a drag-and-drop payload is about to be accepted, the small preview tooltip
    that follows the cursor during a drag, and reading one geometry constant (a border width)
    for resize math. Everything else here is decisions, not pixels.

    One thing that looks like it belongs here but doesn't: text SELECTION inside a window (the
    highlighted, copyable text a user drags over). That lives in chrome instead, because
    selecting text needs to measure actual rendered glyph positions -- a server-level detail
    this unit is not allowed to know about.

    Include order matters: each file can reference statics from files included above it.

    interact/gui_move.c      -- move-drag protocol (move_grab/move_track) + deferred-press latch
    interact/gui_resize.c    -- edge-resize mechanism: hit-test, grab, edge apply
    interact/gui_drag.c      -- drag-and-drop: threshold machine + typed payload (source/target)
    interact/gui_feature.c   -- feat_* kit: window features as freestanding id-keyed mechanisms
    interact/gui_edit.c      -- single-line text edit engine: buffer / cursor / selection / undo,
                                glyph measurement, mouse-drag select, scroll-into-view (no paint)
    interact/gui_edit_multi.c -- multi-line text edit engine: 2D caret, line geometry, undo,
                                mouse-drag select, horizontal pan (region vertical scroll is chrome)
    interact/gui_msel.c      -- multi-select protocol engine: click/modifier rule, range anchor,
                                keyboard extend, one index-range action per frame (caller storage)

==============================================================================================*/

#include <string.h>   /* memset / memcpy / strncmp -- the drag payload slot */

#include "orb.h"
#include "base/fmt.h"         // fmt_snprintf -- the drag payload type tag
#include "base/math.h"        // f32_lerp -- the feat kit's tweens
#include "base/math_ease.h"   // f32_ease_out_cubic -- feat_ease
#include "base/utf8.h"        // codepoint stepping on the caret measure seams (text_x_at / text_offset_at)

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Gestures over the interact server; the style header is here for the WIN_BORDER metric
   read (geometry, not paint -- the documented seam).  No render, no draw. */
#include "runtime_service/gui/gui_host.h"       /* public gui types (-> gui.h -> rect)     */
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/font/gui_font.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*==============================================================================================
    Unity build -- the two raw gesture services first, the drag machine over the same
    arbitration, the feat kit riding move/resize/anim, and the public verbs last.
==============================================================================================*/

#include "runtime_service/gui/interact/gui_move.c"
#include "runtime_service/gui/interact/gui_resize.c"
#include "runtime_service/gui/interact/gui_drag.c"
#include "runtime_service/gui/interact/gui_feature.c"
#include "runtime_service/gui/interact/gui_edit.c"
#include "runtime_service/gui/interact/gui_edit_multi.c"
#include "runtime_service/gui/interact/gui_msel.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  The drag payload slot and the two text-edit undo rings (single-line s_undo
    256 B + multiline s_medit_undo 2 KB) are the real aggregates; the gesture latches (move
    offset, press-defer, resize anchors) are scalar statics, not counted.
==============================================================================================*/

u32
interact_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_drag ) + sizeof( s_undo ) + sizeof( s_medit_undo ) );
}

/*============================================================================================*/
