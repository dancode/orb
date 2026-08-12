/*==============================================================================================

    runtime_service/gui/gui_core.c -- GUI_CORE translation unit: the INTERACT SERVER.

    This is the half of the GUI that answers one question, over and over, for every widget on
    screen: "what is the user doing to this thing right now?" Is the mouse over it? Did it
    just get clicked? Does it have keyboard focus? It reads the raw input -- mouse position,
    key presses -- once per frame and turns it into that answer for whoever asks.

    It also remembers things between frames, which is the "retained-mode storage" half of the
    job. A checkbox's on/off state, a window's position, how far a scroll region has scrolled:
    none of that is recomputed from scratch every frame. It is looked up here by a stable id (a
    hash of the string the caller passed in, e.g. "Save Button"), updated if something changed,
    and left alone otherwise.

    Critically, this unit knows nothing about colors, themes, or how anything looks, and it
    never draws a single pixel. It is one of the two "servers" the rest of the GUI is built on
    top of -- the other is the RENDER SERVER (gui_render.c), which does the opposite: it only
    draws, and knows nothing about clicks or focus. The two never talk to each other directly;
    they meet only inside the frame orchestrator that drives each frame (frame/gui_pane.c).

    The pieces below, roughly in the order they build on each other:

    - An input snapshot that reads raw events from the window/app layer once per frame and
      turns them into a clean, stable reading the rest of the frame works from.
    - The shared scratchpad everything else here builds on: which widget is hovered, which is
      active (pressed or being dragged), which has keyboard focus, plus the pool of
      per-window contexts.
    - Identity: turns a caller's id string into the stable hash number ("id") that keys
      everything else in this file back to that one specific widget.
    - The state pool: the per-widget memory mentioned above, looked up by id and kept across
      frames.
    - Window records: bookkeeping for each open window -- its position, its draw order
      relative to other windows, and which window currently wins the mouse when several
      overlap.
    - Keyboard-focus rules: which widget currently owns the keyboard, plus the rules for
      exclusive input (a text field that should swallow all typing while it is active, say).
    - Keyboard navigation: lets a widget register itself as a tab-stop, so arrow keys and Tab
      can move focus between widgets without the mouse.
    - The item protocol: the common "is this hovered / clicked / dragged" check nearly every
      widget in the GUI runs through, written once here instead of once per widget.
    - Animation helpers: smoothed values and timers (fades, slide-ins) that live here, keyed by
      id just like widget state, so both the style system and gesture code can share them.
    - Query functions: the public "ask a question" API other code calls -- e.g. "does the
      mouse want to click through to the game world right now, or is a widget using it?"

    This unit does not hold the pointers to the app/rhi module APIs it calls into -- those live
    in gui.c, fetched once at startup -- it only reads them, since it owns the boundary where
    raw OS input (events, keys, clipboard, the hardware cursor) enters the GUI.

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
#include "base/utf8.h"        // utf8_encode -- io_add_char encodes typed codepoints into frame text

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
