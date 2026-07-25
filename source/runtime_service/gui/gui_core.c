/*==============================================================================================

    runtime_service/gui/gui_core.c -- GUI_CORE translation unit: the INTERACT SERVER.

    The second server of the two-server model: io routing + dedicated
    retained-mode storage.  It owns the id namespace, the distilled io snapshot, the keyed
    state pool, the ambient interaction record (hover / active / focus), the interaction
    scope, the context pool, the window-record surface service (placement, z dispenser,
    hover/z contest), the standard item protocol, the retained-state animation utilities, and
    the interact query readers.  It answers ONE question for every layer above: "what is the
    user doing to this (id, rect)?"

    It knows nothing of style, themes, or drawing.  It never includes the render server's
    header (render/gui_render.h) -- the two servers meet only in the frame orchestrator (the
    pane bracket, frame/gui_pane.c).  Its few documented upward calls are listed in the
    upward-seams block of core/gui_core.h (draw_nav_ring, nav_scroll_chase + the severable
    debug stamps).

    It does NOT define the module API pointer storage (MOD_USE_RHI / MOD_USE_APP): those
    globals live in gui.c and are fetched once at module init; this unit reads app() through
    the same inline accessors (extern g_*_api_ptr) from app_api.h, since it owns the OS-input
    boundary (event drain, key snapshot, clipboard, hardware cursor).

    Include order matters: each file can reference statics from files included above it.

    core/gui_io.c        -- io snapshot service: app -> io, event drain, io_frame_begin/end, s_io
    core/gui_ctx.c       -- ambient records (s_interaction, s_build, s_scope), the bracketing
                             stacks, the context pool + viewport table, the interact_* verbs,
                             and the per-frame drivers (interact_new_frame, ctx_new_frame)
    core/gui_id.c        -- identity service: id_hash/combine, scope stack, the label grammar
    core/gui_state.c     -- keyed state pool: gui_state_get/peek, the three slot classes
    core/gui_surface.c   -- surface service: window records, placement channel, z dispenser,
                             hover-win contest, surface reassignment slot
    core/gui_focus.c     -- keyboard-focus policy: the exclusive input mode's confine / hold
                             rules, the request latch, the release verb
    core/gui_nav_item.c  -- the keyboard-nav per-item seam: registration into the frame's nav
                             list (the resolver over that list is chrome/nav/gui_nav.c)
    core/gui_item.c      -- the standard item protocol: item_state, item_grab, the compound
                             bracket, and the public door (gui_item / invisible_button)
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

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   The interact server sees the public types, the engine APIs, and its own two headers --
   never render, draw, style, or a policy unit.  debug/gui_debug.h is the one sanctioned
   everywhere-include: severable instrumentation over public types (DBG_* stamps). */

#include "runtime_service/gui/gui_host.h"           /* public gui types (-> gui.h -> rect)     */
#include "runtime_service/rhi/rhi_api.h"            /* rhi handles held by gui_viewport_t      */
#include "engine/app/app_api.h"                     /* app keys / events the io pump speaks    */

#include "runtime_service/gui/core/gui_core.h"      /* the server's services                   */
#include "runtime_service/gui/core/gui_ctx.h"       /* the server's retained-mode storage      */
#include "runtime_service/gui/debug/gui_debug.h"

/*==============================================================================================
    Unity build -- io first (everything reads s_io), then the ambient records and the services
    keyed on them, the item protocol over all of it, and the query readers last.
==============================================================================================*/

#include "runtime_service/gui/core/gui_io.c"
#include "runtime_service/gui/core/gui_ctx.c"
#include "runtime_service/gui/core/gui_id.c"
#include "runtime_service/gui/core/gui_state.c"

#include "runtime_service/gui/core/gui_surface.c"
#include "runtime_service/gui/core/gui_focus.c"
#include "runtime_service/gui/core/gui_nav_item.c"
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
core_unit_mem_bytes( void )
{
    /* s_layout_stack lives in the flow unit -- counted by flow_unit_mem_bytes. */
    return (u32)( sizeof( s_interaction ) + sizeof( s_build ) + sizeof( s_scope )
                + sizeof( s_io ) + sizeof( s_click_elapsed )
                + sizeof( s_click_x ) + sizeof( s_click_y )
                + sizeof( s_item_flag_stack )
                + sizeof( s_id_stack ) + sizeof( s_ctx_pool ) );
}

/*============================================================================================*/
