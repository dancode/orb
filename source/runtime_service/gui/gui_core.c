/*==============================================================================================

    runtime_service/gui/gui_core.c -- GUI_CORE translation unit: the INTERACT SERVER.

    The second server of the two-server model (GUI_SERVER_PLAN.md): io routing + dedicated
    retained-mode storage.  It owns the id namespace, the distilled io snapshot, the keyed
    state pool, the ambient interaction record (hover / active / focus), the interaction
    scope, the context pool, the window-record surface service (placement, z dispenser,
    hover/z contest), the standard item protocol, the retained-state animation utilities, and
    the interact query readers.  It answers ONE question for every layer above: "what is the
    user doing to this (id, rect)?"

    It knows nothing of style, themes, or drawing.  It never includes the render server's
    header (render/gui_render.h) -- the two servers meet only in the frame orchestrator (the
    pane bracket, frame/gui_pane.c).  Its few documented upward calls are listed in the
    upward-seams block of core/gui_core.h (draw_nav_ring + the severable debug stamps).

    It does NOT define the module API pointer storage (MOD_USE_RHI / MOD_USE_APP): those
    globals live in gui.c and are fetched once at module init; this unit reads app() through
    the same inline accessors (extern g_*_api_ptr) from app_api.h -- the io half owns the
    OS-input boundary (event drain, key snapshot, clipboard, hardware cursor).

    Include order matters: each file can reference statics from files included above it.

    core/gui_io.c        -- io snapshot service: app -> io, event drain, io_frame_begin/end, s_io
    core/gui_ctx.c       -- ambient records (s_interaction, s_build, s_scope), context pool
                             storage, per-frame drivers (ctx_new_frame, interaction_frame_reset)
    core/gui_id.c        -- identity service: id_hash/combine, scope stack, the label grammar
    core/gui_state.c     -- keyed state pool: gui_state_get/peek, the three slot classes
    core/gui_surface.c   -- surface service: window records, placement channel, z dispenser,
                             hover-win contest, surface reassignment slot
    core/gui_item.c      -- the standard item protocol: item_state, item_grab, nav registration
    core/gui_anim.c      -- retained-state animation utilities: dampers, timers (keyed-state
                             tenants -- server-side so style blends and interact tweens share them)
    core/gui_query.c     -- the public interact query readers: want_capture_*, is_item_*, is_key_*

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"        // f32_lerp -- from/to interpolation for the animation service
#include "base/math_ease.h"   // f32_ease_* shapers -- the easing curves the animation service applies

#include "runtime_service/gui/gui_internal.h"   /* umbrella: unit headers in stack order */

/*==============================================================================================
    Unity build -- io first (everything reads s_io), then the ambient records and the services
    keyed on them, the item protocol over all of it, and the query readers last.
==============================================================================================*/

#include "runtime_service/gui/core/gui_io.c"
#include "runtime_service/gui/core/gui_ctx.c"
#include "runtime_service/gui/core/gui_id.c"
#include "runtime_service/gui/core/gui_state.c"

#include "runtime_service/gui/core/gui_surface.c"
#include "runtime_service/gui/core/gui_item.c"
#include "runtime_service/gui/core/gui_anim.c"

#include "runtime_service/gui/core/gui_query.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  The per-context retained state is heap (counted as context blocks by
    gui_mem_stats); what lives here is the ambient records, the io snapshot, the bracketing
    stacks, and the context pool's pointer array.
==============================================================================================*/

u32
gui_core_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_interaction ) + sizeof( s_build ) + sizeof( s_scope )
                + sizeof( s_io ) + sizeof( s_click_elapsed )
                + sizeof( s_click_x ) + sizeof( s_click_y )
                + sizeof( s_item_flag_stack ) + sizeof( s_layout_stack )
                + sizeof( s_id_stack ) + sizeof( s_ctx_pool ) );
}

/*============================================================================================*/
