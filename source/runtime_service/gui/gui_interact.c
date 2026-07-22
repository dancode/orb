/*==============================================================================================

    runtime_service/gui/gui_interact.c -- GUI_INTERACT translation unit: gesture mechanisms.

    The library of record-agnostic interaction elements over the interact server
    (GUI_SERVER_PLAN.md): move-drag with deferred press, edge resize, drag-and-drop payload
    transfer, the feat_* window feature kit, and the public behavior verbs.  Every mechanism
    consumes (id, rect, io) plus caller-owned state and produces DECISIONS -- new geometry,
    gesture liveness, a delivered payload.  None knows a widget, and none paints.

    This unit and core/ are the ONLY writers of the s_interaction arbitration fields
    (hover / active / focus): higher tiers claim through the core verbs (interact_claim)
    and read the record for gating, never write it raw.

    ACCEPTANCE: no render header.  The documented exceptions live in the upward-seams block
    of interact/gui_interact.h (mirroring core's): draw_drop_ring -- the ONE adornment paint,
    invoked where the accept is decided; the drag preview tooltip through the public chrome
    verbs; and resize's WIN_BORDER read (geometry, not paint).

    NOT here: window text selection (chrome/window/gui_select.c since R6) -- its protocol reads the
    render server's run capture and measures with draw-unit font metrics, server crossings
    this unit must never make; it is chrome policy riding this unit's generic verbs.

    Include order matters: each file can reference statics from files included above it.

    interact/gui_move.c      -- move-drag protocol (move_grab/move_track) + deferred-press latch
    interact/gui_resize.c    -- edge-resize mechanism: hit-test, grab, edge apply
    interact/gui_drag.c      -- drag-and-drop: threshold machine + typed payload (source/target)
    interact/gui_feature.c   -- feat_* kit: window features as freestanding id-keyed mechanisms
    interact/gui_behavior.c  -- public behavior on caller rects: gui_item, invisible_button

==============================================================================================*/

#include <string.h>   /* memset / memcpy / strncmp -- the drag payload slot */

#include "orb.h"
#include "base/fmt.h"         // fmt_snprintf -- the drag payload type tag
#include "base/math.h"        // f32_lerp -- the feat kit's tweens
#include "base/math_ease.h"   // f32_ease_out_cubic -- feat_ease

/* This unit's world, and nothing above it (R11: the include list IS the dependency graph).
   Gestures over the interact server; the style header is here for the WIN_BORDER metric
   read (geometry, not paint -- the R6-documented seam).  No render, no draw. */
#include "runtime_service/gui/gui_host.h"       /* public gui types (-> gui.h -> rect)     */
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
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
#include "runtime_service/gui/interact/gui_behavior.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  The drag payload slot is the one real aggregate; the gesture latches
    (move offset, press-defer, resize anchors) are scalar statics, not counted by contract.
==============================================================================================*/

u32
gui_interact_unit_mem_bytes( void )
{
    return (u32)sizeof( s_drag );
}

/*============================================================================================*/
