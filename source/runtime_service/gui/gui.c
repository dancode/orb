/*==============================================================================================

    runtime_service/gui/gui.c -- Unity build entry for the gui FRAME unit (the orchestrator).

    gui is multiple translation units linked into one static lib (GUI_SERVER_PLAN.md).
    Cross-unit reach goes through the ambient-record externs and service seams in the per-unit
    headers under gui_internal.h, so every library boundary is compiler-enforced:

      - this unit (gui.c): the FRAME ORCHESTRATOR -- frame lifecycle, the pane bracket,
        the module vtable.  It boots both servers, pumps io into the interact server, and
        hands contexts to the render server.

      - the interact unit (gui_interact.c, R6): gesture mechanisms -- move/resize/drag,
        the feat_* kit, the public behavior verbs.  Decides, never paints.  Reached
        through the interact/gui_interact.h seams.

      - the interact server (gui_core.c, R4): io snapshot, ids, keyed state, the ambient
        interaction records, the surface service, the item protocol, anim, query readers.
        Owns s_build / s_scope / s_io / s_interaction / g_ctx and the core stacks.

      - the render server (gui_render.c): draw list, tessellation, atlas, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_render.  Called through gui_render.h.

      - the style unit (gui_style.c, R5): theme registry, style stacks, lattice, state ->
        color projections.  Interact state in as parameters, colors/metrics out; never
        paints.  Reached through the style/gui_style.h seams.

      - the draw unit (gui_draw.c, R3): drawing routines + font/icon resources over the
        render server's primitives.

      - the element unit (gui_element.c, R8): the styled building blocks astride both
        servers -- the el_* rect-consuming cores, the per-item ambient wrappers + system
        adornments (element/gui_adornment.c), and the styled symbol half.

      - the flow unit (gui_flow.c, R7): composition -- spacing metrics in, rects out.
        Upward seams: scrollbar_widget + the child box paint trio, nothing else.

      - the chrome unit (gui_chrome.c): widgets/ + table/ + window/ + dock/ + popup/ + nav/
        over the core services and flow's emit surface; its frame-called steps (raise-on-
        press, modal fence, nav turnover, dock upkeep) are seams back the other way.

      - the debug unit (gui_debug.c, R10): pipeline dashboard + command stepper; severable.

    Include order in this unit matters: each file can reference statics from files included
    above it.  What remains is the frame group (incl. the pane bracket + context lifecycle).

    render/     -- beside the stack, not a rung in it: the RENDER SERVER (gui_render.c),
                    reached only through gui_render.h at flush.
    core/        -- the INTERACT SERVER (its own TU, gui_core.c, since R4): the ambient
                    context records, identity, keyed state, the io snapshot, the surface
                    service (window records, z dispenser, hover contest), the item protocol,
                    anim, and the query readers.  This unit reaches it through the
                    core/gui_core.h + core/gui_ctx.h seams.
    style/       -- THE STYLE UNIT (its own TU, gui_style.c, since R5): theme registry,
                    style stacks, lattice, state -> color projections, the bracketing
                    vocabulary.  Not in this TU; reached through the style/gui_style.h seams.
    flow/        -- THE FLOW UNIT (its own TU, gui_flow.c, since R7): the only code that
                    turns style spacing metrics into rects.  Not in this TU.
    interact/    -- THE INTERACT UNIT (its own TU, gui_interact.c, since R6): the gesture
                    mechanisms -- drag threshold + payload, move-drag + deferred-press,
                    edge resize, the feat_* kit, the public behavior verbs.  Not in this TU.
                    Each service is a capability (exclusivity, clicks, tracking) over
                    (id, rect); none knows a widget, and none paints.  interact/ + core/
                    stay the ONLY writers of the s_interaction arbitration fields
                    (hover/active/focus): higher tiers claim through the core verbs
                    (interact_claim) and read the record for gating, never write it raw.
                    Window text selection re-classified as chrome (chrome/window/gui_select.c, R6).
    element/     -- THE ELEMENT UNIT (its own TU, gui_element.c, since R8): presentation
                    -- consumes rect + state + skin and paints; never asks behavior, state
                    is a parameter.  The el_* cores, the per-item ambient wrappers, the
                    system adornments, the styled symbol half.  Not in this TU.
                    (present/ dissolved into it at R8.)  flow/, interact/, element/ are
                    SIBLINGS -- combined only by the tiers above.
    chrome/      -- THE CHROME UNIT (gui_chrome.c): the stock widget set + the host
                    structures; its six folders (widgets, table, window, dock, popup, nav)
                    live under it since R9.  Not in this TU; see gui_chrome.c for the role map.
    (user/ dissolved at R6 -- the caller's vocabulary lives with its machinery: canvas ->
     draw (R3), query readers -> core (R4), bracketing stacks -> style (R5), behavior verbs
     -> interact (R6).  Where user widgets are written: rect (canvas) + item() + draw_*.)
    debug/       -- dev tooling: dashboard + stepper are THE DEBUG UNIT (its own TU,
                    gui_debug.c, since R10 at the root like every unit).  Severable.
    frame/       -- the conductor: gui_frame.c lifecycle, gui_viewport.c, gui_boot.c.  Top
                    of the stack (the host's driver over the tiers), NOT a foundation:
                    prepare/update/dispatch touches every tier.
    (root)       -- this unity entry, the public headers, and gui_api.c (vtable, mod_desc).

    Cross-role contract (the composer / behavior / presentation split):
      composition  (flow/)     consumes spacing metrics, produces rects;
      behavior     (interact/) consumes (id, rect), produces interaction state;
      presentation (element/)  consumes rect + state + skin and paints.
    A widget (the chrome unit) is the only combiner: it asks composition for a rect, hands it to
    behavior, hands both results to presentation.  The public gui_item/canvas/draw_* verbs are
    the caller's door onto the same roles, skin optional -- the game-UI path.

    THIS UNIT's constituents (the carved units list their own; the interact server's former
    residents live in gui_core.c since R4; the style machinery in gui_style.c since R5; the
    gesture services in gui_interact.c since R6; the layout composer in gui_flow.c since R7;
    the ambient wrappers + adornments in gui_element.c since R8):

    frame/gui_frame_overlay.c    -- built-in perf / state HUD overlays + the frame-timing helpers they read
                                      (home since R10 -- conductor code, never part of the debug unit)

    frame/gui_frame.c            -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, font, clip
    frame/gui_viewport.c         -- viewport open/resize/close + gui-owned floater lifecycle (spawn/update/render_floaters)
    frame/gui_boot.c             -- one-call host front end: boot, frame_poll, present_begin/present
    frame/gui_pane.c             -- the pane bracket: pane_tag + gui_pane_begin/end stamp BOTH servers (R4)
    frame/gui_context.c          -- public multi-context lifecycle over the core pool (R4)

    gui_ui_mem.c                 -- frontend memory accounting (gui_ui_memory) + the gui_mem_stats
                                      aggregation; must be the last constituent include so it sees them all
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
#include "runtime_service/gui/render/gui_render.h"

// API function headers + access pointers -- wired at startup.
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"
MOD_USE_RHI;
MOD_USE_APP;

// clang-format off
/*==============================================================================================
    Debug Overlay

    The debug-overlay build switch (GUI_DEBUG_OVERLAY) and the DBG_* capture macros live in
    gui_render.h: both units (this one and gui_render.c, which defines the capture targets)
    must agree on them.  The widget / chrome files below invoke DBG_WIDGET / DBG_WINDOW / DBG_RESIZE;
    in Debug they call across to the overlay's capture functions in the backend unit.

    #define GUI_DEBUG_OVERLAY 1    -- currently auto enabled in Debug builds 
    
    see: gui_render.h for the capture macros

==============================================================================================*/

/*==============================================================================================
    Capability flags -- latched by gui_init_config_front (gui_frame.c), read directly (same TU)
    by any file below that owns an optional feature boundary: gui_table.c (tables),
    gui_dock*.c (docking), gui_nav.c (keyboard_nav).  Declared here, before every tier include, so
    all of them see it -- the gui_render.c s_caps placement, mirrored for this unit.  A compound
    literal is not a valid static initializer (see gui_frame.c's s_init_caps comment), so this
    repeats GUI_FORWARD_CAPS_DEFAULT's fields by hand; gui_init_config_front overwrites it before
    init().
==============================================================================================*/

gui_forward_caps_t s_fwd_caps = { .tables = true, .docking = true, .keyboard_nav = true };

/* The theme registry, base/active style state (s_style_base, s_style, s_font_size), the style
   stacks, and layout_compute live in the STYLE UNIT (gui_style.c) since R5; this unit reads
   s_style and the resolvers through the style/gui_style.h externs + seams.

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

/* The render backend (render/resource/gui_atlas, gui_font, gui_icon; render/pipeline/gui_shader,
   gui_emit_draw, gui_emit_path, gui_build_tess, gui_build_volatile, gui_build_cache, gui_render;
   render/gui_debug_overlay) is the SECOND unit -- compiled separately via gui_render.c.  This
   unit calls into it through the draw_* / font_* / gui_render_* declarations in gui_render.h. */

/*----------------------------------  LIBRARY: GUI_CORE  ----------------------------------*/
// THE INTERACT SERVER is its OWN translation unit since R4 (gui_core.c): io, ids, keyed
// state, ambient interaction records, the surface service, the item protocol, anim, and the
// query readers.  This unit reaches it through the core/gui_core.h + core/gui_ctx.h seams.
// THE STYLE UNIT is its own translation unit since R5 (gui_style.c): theme registry, stacks,
// resolution, projections -- reached through the style/gui_style.h seams.
// The present/ paint primitives (the last of the old core group) moved to the element unit
// at R8 (element/gui_adornment.c) and present/ is DISSOLVED.

/* gui_symbol.c moved to the draw unit (gui_draw.c, R3); the gesture services (gui_move.c,
   gui_resize.c, gui_drag.c, gui_feature.c, gui_behavior.c) moved to the interact unit
   (gui_interact.c, R6); window text selection was re-classified as chrome and moved to
   chrome/window/gui_select.c (the chrome unit) -- it reads the render capture + font metrics. */

/*----------------------------------  LIBRARY: GUI_FLOW  ----------------------------------*/

// GUI_FLOW is its OWN translation unit since R7 (gui_flow.c): composition -- spacing
// metrics in, rects out.  It reaches core through the flow/gui_flow.h + core seams; its
// upward calls (scrollbar_widget, the child box paint trio) are the documented block in
// flow/gui_flow.h.  This unit calls INTO it through the same seam declarations.

/*----------------------------------  LIBRARY: GUI_INTERACT  ----------------------------------*/
/* GUI_INTERACT is its OWN translation unit since R6 (gui_interact.c): move/resize/drag
   gestures, the feat_* kit, and the public behavior verbs.  This unit reaches them through
   the interact/gui_interact.h seams (frame_begin drives drag_new_frame; the viewport
   tear-off reads move_grab_offset). */

/*----------------------------------  LIBRARY: GUI_CHROME  ----------------------------------*/
// GUI_CHROME is its OWN translation unit (gui_chrome.c): the six folders under chrome/
// since R9 -- widgets, table, window, dock, popup, nav (nav is core-classified but reads
// the popup stack, so it lives with chrome).  It composes the core services + the flow emit surface through the
// gui_internal.h seams; this unit's upward calls into it (the frame lifecycle's window /
// popup / dock / nav steps) resolve through the same seam declarations.

/* user/ dissolved at R6: gui_canvas.c -> draw (R3); gui_query.c -> the interact server (R4);
   gui_stacks.c -> the style unit (R5); gui_behavior.c -> the interact unit (R6). */

// element/ -- GUI_ELEMENT is its OWN translation unit since R8 (root gui_element.c): the
// el_* cores plus the absorbed styled painters (per-item wrappers, system adornments, the
// styled symbol half), reached through element/gui_element_internal.h.
// gui_style_apply (frame/, below) calls across to el_style_derive at every theme/font landing.

/*----------------------------------  LIBRARY: GUI_DEBUG  ----------------------------------*/

// GUI_DEBUG is its OWN translation unit (root gui_debug.c since R10): the pipeline dashboard
// + command stepper reach gui only through the public surface, the backend capture API, and
// the gui_internal.h seams.  gui_frame_overlay.c stays in THIS unit (frame group below): it
// carries the frame-timing helpers the lifecycle calls -- conductor code, not severable
// tooling -- and lives in frame/ since R10.

/*----------------------------------  LIBRARY: GUI_FRAME  ----------------------------------*/

// Orchestration -- sits above every tier, drives whichever are compiled in.  The overlay file
// carries the perf/state HUDs plus the frame-timing helpers the lifecycle in gui_frame.c calls,
// so it must precede gui_frame.c in the unity build.
#include "runtime_service/gui/frame/gui_frame_overlay.c"
#include "runtime_service/gui/frame/gui_frame.c"

// The pane bracket -- the go-between verb stamping BOTH servers (R4); and the public
// multi-context lifecycle -- context destruction tears down GPU surfaces, orchestrator work.
#include "runtime_service/gui/frame/gui_pane.c"
#include "runtime_service/gui/frame/gui_context.c"

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