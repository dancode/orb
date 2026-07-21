/*==============================================================================================

    runtime_service/gui/gui.c -- Unity build entry for the gui CORE + FRAME unit.

    gui is SIX translation units linked into one static lib (the per-library TU split,
    docs/GUI_STACK_PLAN.md inc 10).  Cross-unit reach goes through the ambient-record externs
    and service seams in gui_internal.h, so every library boundary is compiler-enforced:

      - this unit (gui.c): the interaction server + the conductor -- context, ids, keyed
        state, io snapshot, style machinery, surface service, the interact/ gesture services,
        present/ paint primitives, the user/ vocabulary, frame lifecycle, the module vtable.
        Owns s_build / s_scope / s_io / s_interaction / g_ctx and the stacks.

      - the render backend (gui_backend.c): fonts, draw list, tessellation, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_font / s_render.  Called through gui_backend.h.

      - the element unit (element/gui_element.c): the el_* rect-consuming widget cores;
        public surface + the style_active() seam only.

      - the flow unit (compose/gui_flow.c): composition -- spacing metrics in, rects out.
        Upward seams: scrollbar_widget + the gui_anim_* ease, nothing else.

      - the chrome unit (gui_chrome.c): widgets/ + table/ + window/ + dock/ + popup/ + nav/
        over the core services and flow's emit surface; its frame-called steps (raise-on-
        press, modal fence, nav turnover, dock upkeep) are seams back the other way.

      - the debug unit (debug/gui_debug.c): pipeline dashboard + command stepper; severable.

    Include order in this unit matters: each file can reference statics from files included
    above it.  Directories name ROLES, not rungs; a role only depends on roles included above
    it.  Order here: core -> surface -> present / interact (siblings) -> flow's seam gap ->
    anim + feat kit -> user -> frame.

    backend/     -- beside the stack, not a rung in it: the render sink (the second TU,
                    gui_backend.c), reached only through gui_backend.h at flush.
    core/  -- machinery with no opinions: the ambient context records (s_interaction,
                    s_build, s_scope, g_ctx), identity (ids), keyed state tracking, the io
                    snapshot, and the style machinery (theme registry, stacks, resolution).
                    Owns style MACHINERY, never style MEANING -- no core decision reads
                    a style value.
    surface/     -- the surface service: window records as placed, stacked, occluding
                    rectangles -- the pool (window_get/window_find), the next-window
                    placement channel, the z dispenser (surface_z_raise, this tier is its
                    ONLY writer), the hover-win occlusion contest (surface_hover_nominate),
                    the surface reassignment request slot, and open/closed state.  No
                    layout, no chrome, no gestures -- those are window/ policy over these
                    services.  Storage + frame turnover stay with the context
                    (core/gui_ctx.c), the house pattern; viewport (OS surface)
                    lifecycle stays with the conductor (app()/rhi() operations).
    compose/     -- composition (THE FLOW UNIT, compose/gui_flow.c): the only code that turns
                    style spacing metrics into rects.  Not in this TU.
    interact/    -- behavior: widget-agnostic interaction services -- the standard item
                    protocol (item_state), bare chrome grab (item_grab), drag threshold +
                    payload, move-drag + deferred-press (move_grab/move_track, press_defer_*),
                    edge-resize mechanism (resize_item), animation stepping.  Each serves a
                    capability (exclusivity, clicks, tracking) over (id, rect); none knows a
                    widget, and none reads a style value or paints -- system adornments (nav
                    ring, drop ring, hot edges) are invoked from here but painted by present/
                    helpers.  This tier is the ONLY writer of the s_interaction arbitration
                    fields (hover/active/focus): higher tiers claim through these verbs and
                    read the record for gating, never write it raw -- the popup modal fence
                    claims through interact_hover_fence.  Behavior's only inputs beyond (id, rect)
                    are the interaction scope (s_scope: owner window, clip, chrome
                    suppression, per-item flag/nav stamps) -- stamped by composition at its
                    seams -- and its own s_interaction; it never reads the composer scratch
                    (s_build).
    present/     -- presentation: the shared paint primitives -- COL_* palette, widget
                    macros, label grammar, text-fit, symbol/shape draws.  Consumes
                    rect + state + skin; never asks behavior, state is a parameter.
                    compose/, interact/, present/ are SIBLINGS -- combined only by the
                    tiers above.
    widgets/ table/ window/ dock/ popup/ nav/
                 -- THE CHROME UNIT (gui_chrome.c): the stock widget set + the host
                    structures.  Not in this TU; see gui_chrome.c for the role map.
    user/        -- the caller's vocabulary: pure public verbs + readers -- the bracketing
                    stacks (id / item flags / style / scale / disabled), behavior on caller
                    rects (gui_item), the canvas + raw-draw surface, and the query readers
                    (want_capture_*, is_item_*, is_key_*).  Zero state, zero machinery;
                    consumed only from outside the lib (via the vtable) or by lower tiers
                    deliberately dogfooding the public surface through gui_host.h declarations.
                    Where user widgets are written: rect (canvas) + item() + draw_*, no skin.
    debug/       -- dev tooling: dashboard + stepper are THE DEBUG UNIT (debug/gui_debug.c);
                    gui_frame_overlay.c stays HERE (the lifecycle calls its timing helpers).
    frame/       -- the conductor: gui_frame.c lifecycle, gui_viewport.c, gui_boot.c.  Top of
                    the stack alongside user/ (the host's driver over the tiers, as user/ is
                    the caller's door into them), NOT a foundation: prepare/update/dispatch
                    touches every tier.
    (root)       -- this unity entry, the public headers, and gui_api.c (vtable, mod_desc).

    Cross-role contract (the composer / behavior / presentation split):
      composition  (compose/)  consumes spacing metrics, produces rects;
      behavior     (interact/) consumes (id, rect), produces interaction state;
      presentation (present/)  consumes rect + state + skin and paints.
    A widget (the chrome unit) is the only combiner: it asks composition for a rect, hands it to
    behavior, hands both results to presentation.  user/ is the public door onto the same
    roles, skin optional -- the game-UI path.

    THIS UNIT's constituents (the carved units list their own):

    core/gui_theme.c       -- theme registry + base/active style state, theme API, layout_compute
    core/gui_style.c       -- style stacks machinery: style_col/style_var resolution, push/pop/next ops
    core/gui_ctx.c         -- context state: s_interaction, s_build, s_scope, layout_frame_t, gui_context_t,
                                      ctx_new_frame, memory stats, multi-context lifecycle (ctx_create/destroy/bind)
    core/gui_io.c          -- io snapshot service: app->IO, io_frame_begin/end, s_io
    core/gui_id.c          -- identity service: id_hash, id_combine, id_seed/push/pop
    core/gui_state.c       -- keyed state tracking service: gui_state_get/peek, GUI_STATE

    surface/gui_surface.c        -- surface service: window record pool, placement channel, z dispenser,
                                      hover-win contest, surface reassignment slot, open/closed state

    interact/gui_item.c          -- the standard item protocol: item_state, item_grab, nav registration, repeat
    interact/gui_drag.c          -- drag service: threshold machine + typed payload transfer (source/target)
    interact/gui_move.c          -- move-drag protocol (move_grab/move_track) + deferred-press latch (press_defer_*)
    interact/gui_resize.c        -- edge-resize mechanism: resize_item protocol, hit-test, grab, edge apply
    interact/gui_anim.c          -- animation stepping service: gui_anim_f32

    present/gui_paint_core.c    -- shared presentation primitives: COL_* palette, layout macros, label
                                      grammar, system adornments (nav/drop rings, resize highlight)
    present/gui_symbol.c         -- symbol + shape draw primitives: draw_arrow/check/frame/round_rect/arc/...

    user/gui_stacks.c            -- bracketing vocabulary: push/pop id, item flags, style color/var, scale, disabled
    user/gui_behavior.c          -- public behavior on caller rects: gui_item, invisible_button
    user/gui_canvas.c            -- custom-draw surface: canvas, draw_rect/text, text measure, icons
    user/gui_query.c             -- public readers: want_capture_*, is_item_*, is_key_*, is_mouse_*, get_mouse_pos

    debug/gui_frame_overlay.c    -- built-in perf / state HUD overlays + the frame-timing helpers they read

    frame/gui_frame.c            -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, font, clip
    frame/gui_viewport.c         -- viewport open/resize/close + gui-owned floater lifecycle (spawn/update/render_floaters)
    frame/gui_boot.c             -- one-call host front end: boot, frame_poll, present_begin/present

    gui_ui_mem.c                 -- frontend memory accounting: gui_ui_memory sizeof-sums this unit's
                                      statics; must be the last constituent include so it sees them all
    gui_api.c                    -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h> /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>   /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"
#include "base/fmt.h"         // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting on the per-frame text paths
#include "base/math.h"        // f32_lerp -- from/to interpolation for the animation service
#include "base/math_ease.h"   // f32_ease_* shapers -- the easing curves the animation service applies

#include "engine/sys/sys_host.h"   // sys_root_dir -- disk assets (load_icon, asset_path) resolve root-relative

// API GUI render-backend
#include "runtime_service/gui/gui_backend.h"

// API function headers + access pointers -- wired at startup.
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"
MOD_USE_RHI;
MOD_USE_APP;

// clang-format off
/*==============================================================================================
    Debug Overlay

    The debug-overlay build switch (GUI_DEBUG_OVERLAY) and the DBG_* capture macros live in
    gui_backend.h: both units (this one and gui_backend.c, which defines the capture targets)
    must agree on them.  The widget / chrome files below invoke DBG_WIDGET / DBG_WINDOW / DBG_RESIZE;
    in Debug they call across to the overlay's capture functions in the backend unit.

    #define GUI_DEBUG_OVERLAY 1    -- currently auto enabled in Debug builds 
    
    see: gui_backend.h for the capture macros

==============================================================================================*/

/*==============================================================================================
    Capability flags -- latched by gui_init_config_front (gui_frame.c), read directly (same TU)
    by any file below that owns an optional feature boundary: gui_table.c (tables),
    gui_dock*.c (docking), gui_nav.c (keyboard_nav).  Declared here, before every tier include, so
    all of them see it -- the gui_backend.c s_caps placement, mirrored for this unit.  A compound
    literal is not a valid static initializer (see gui_frame.c's s_init_caps comment), so this
    repeats GUI_FORWARD_CAPS_DEFAULT's fields by hand; gui_init_config_front overwrites it before
    init().
==============================================================================================*/

gui_forward_caps_t s_fwd_caps = { .tables = true, .docking = true, .keyboard_nav = true };

/* The theme registry, base/active style state (s_style_base, s_style, s_font_size), the theme
   API, and layout_compute now live in core/gui_theme.c -- included first among core/ below, so
   s_style is declared before core/gui_style.c's push-stack resolvers (and every later tier) read
   it in this TU.

   The shared stateless helpers (saturate, clampf, rect_intersect) live in gui_internal.h as
   static inline -- both units use them (gui_emit_draw.c needs rect_intersect for clip nesting). */

/*==============================================================================================
    Internal record types shared into gui_context_t

    The per-context record types (gui_window_t, layout_frame_t, gui_popup_t, gui_viewport_t,
    gui_context_t, ...) and the cross-unity-boundary forward declarations now live in
    gui_internal.h, included at the top of this file -- so every constituent file below sees the
    full shared type layer up front rather than relying on include order.  Their behavior stays in
    the owning .c file.
==============================================================================================*/

/*==============================================================================================
    Unity build
==============================================================================================*/

/* The render backend (backend/resource/gui_atlas, gui_font, gui_icon; backend/pipeline/gui_shader,
   gui_emit_draw, gui_emit_path, gui_build_tess, gui_build_volatile, gui_build_cache, gui_render;
   backend/gui_debug_overlay) is the SECOND unit -- compiled separately via gui_backend.c.  This
   unit calls into it through the draw_* / font_* / gui_render_* declarations in gui_backend.h. */

/*----------------------------------  LIBRARY: GUI_CORE  ----------------------------------*/
// core/ + surface/ + interact/ + present/ paint primitives.  NOTE the cross-cut: the
// present/ files are gui_draw-classified but read resolved style -- they stay beside core
// until the style purge (GUI_STACK_PLAN inc 5) parameterizes them.
// Core machinery interleaved with the sibling roles (present/ primitives, interact/
// services): the include order is dependency-driven, the directories are the conceptual split.
#include "runtime_service/gui/core/gui_theme.c"
#include "runtime_service/gui/core/gui_io.c"
#include "runtime_service/gui/core/gui_style.c"
#include "runtime_service/gui/core/gui_ctx.c"
#include "runtime_service/gui/core/gui_id.c"
#include "runtime_service/gui/core/gui_state.c"

#include "runtime_service/gui/surface/gui_surface.c"

#include "runtime_service/gui/present/gui_paint_core.c"
#include "runtime_service/gui/interact/gui_item.c"
#include "runtime_service/gui/interact/gui_drag.c"
#include "runtime_service/gui/interact/gui_move.c"
#include "runtime_service/gui/present/gui_symbol.c"
#include "runtime_service/gui/interact/gui_resize.c"

// Window text selection (GUI_WIN_TEXT_SELECT): press/sweep protocol, highlight paint, Ctrl+C
// copy over the backend's captured runs (backend/gui_select_capture.c).  Here in interact/ --
// it writes s_interaction (a fallback press consumer) -- called from window/gui_window_end.c.
#include "runtime_service/gui/interact/gui_select.c"

/*----------------------------------  LIBRARY: GUI_FLOW  ----------------------------------*/

// GUI_FLOW is its OWN translation unit (compose/gui_flow.c, the fifth): composition --
// spacing metrics in, rects out.  It reaches core through the ambient-record externs +
// service seams in gui_internal.h; its two upward calls (scrollbar_widget, gui_anim_f32)
// are seams there too.  This unit calls INTO it through the same seam declarations.

/*----------------------------------  LIBRARY: GUI_CORE  ----------------------------------*/
// the animation service (placed here in static-visibility order).
#include "runtime_service/gui/interact/gui_anim.c"
// the feat_* kit: window features as freestanding id-keyed mechanisms (rides move/resize/anim).
#include "runtime_service/gui/interact/gui_feature.c"

/*----------------------------------  LIBRARY: GUI_CHROME  ----------------------------------*/
// GUI_CHROME is its OWN translation unit (gui_chrome.c, the sixth): widgets/ + table/ +
// window/ + dock/ + popup/ + nav/ (nav is core-classified but reads the popup stack, so it
// lives with chrome).  It composes the core services + the flow emit surface through the
// gui_internal.h seams; this unit's upward calls into it (the frame lifecycle's window /
// popup / dock / nav steps) resolve through the same seam declarations.

/*----------------------------------  LIBRARY: GUI_CORE  ----------------------------------*/
// user/ -- the public door.  NOTE the cross-cut: gui_stacks.c mixes core brackets
// (id / item flags / disabled) with chrome style stacks; gui_canvas.c is draw-facing.
// user/ -- the caller's vocabulary.  Pure public verbs + readers over the machinery
// above; zero state, zero machinery -- deleting any file here breaks no lower tier.  Included
// last-of-the-tiers because nothing below needs them at definition time (all upward calls go
// through gui_host.h declarations).
#include "runtime_service/gui/user/gui_stacks.c"
#include "runtime_service/gui/user/gui_behavior.c"
#include "runtime_service/gui/user/gui_canvas.c"
#include "runtime_service/gui/user/gui_query.c"

// element/ -- GUI_ELEMENT is its OWN translation unit (element/gui_element.c): the compiler
// enforces that the element tier reaches gui only through the public gui_* surface + the
// style_active() seam (gui_internal.h).
// gui_style_apply (frame/, below) calls across to el_style_derive at every theme/font landing.

/*----------------------------------  LIBRARY: GUI_DEBUG  ----------------------------------*/

// GUI_DEBUG is its OWN translation unit (debug/gui_debug.c, the fourth beside this one,
// gui_backend.c, and element/gui_element.c): the pipeline dashboard + command stepper reach
// gui only through the public surface, the backend capture API, and the gui_internal.h seams.
// gui_frame_overlay.c stays HERE (frame group below): it carries the frame-timing helpers
// the lifecycle calls -- conductor code, not severable tooling.

/*----------------------------------  LIBRARY: GUI_FRAME  ----------------------------------*/

// Orchestration -- sits above every tier, drives whichever are compiled in.  The overlay file
// carries the perf/state HUDs plus the frame-timing helpers the lifecycle in gui_frame.c calls,
// so it must precede gui_frame.c in the unity build.
#include "runtime_service/gui/debug/gui_frame_overlay.c"
#include "runtime_service/gui/frame/gui_frame.c"

// Viewport lifecycle + gui-owned floater surfaces -- separated from gui_frame.c because it is a
// distinct concern (OS window / rhi context ownership) from the frame lifecycle proper.  Included
// after gui_frame.c: gui_viewport_render_floaters calls gui_render(), defined there.
#include "runtime_service/gui/frame/gui_viewport.c"

// Boot-tier host front end -- one-call setup (boot) + the canonical loop (frame_poll,
// present_begin/present).  Last: it composes the lifecycle, viewport, and window layers above.
#include "runtime_service/gui/frame/gui_boot.c"

// MEMORY ACCOUNTING: sizeof-sums this unit's fixed statics for gui_mem_stats (cpu_frontend_bytes).
// MUST stay the last constituent include -- unity visibility only flows downward, and the
// full-accounting contract is that every static aggregate above is in scope here.
#include "runtime_service/gui/gui_ui_mem.c"

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
// clang-format on