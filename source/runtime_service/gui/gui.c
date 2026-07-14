/*==============================================================================================

    runtime_service/gui/gui.c -- Unity build entry for the gui UI / core unit.

    gui is two translation units linked into one static lib (see gui_backend.h):

      - this unit (gui.c): context, layout, widgets, chrome, popups, nav, input, frame
        lifecycle, the module vtable.  Owns s_build / s_scope / s_io / s_interaction / g_ctx
        and the stacks.

      - the render backend (gui_backend.c): fonts, draw list, tessellation, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_font / s_render.  Called through gui_backend.h.
    
    Include order matters: each file can reference statics from files included above it.  The
    include list below is THE dependency order, the one the compiler enforces -- directories
    name ROLES, not rungs.  A role only depends on roles included above it, and a role dir is
    also where a future `#ifdef GUI_ENABLE_<role>` would wrap an #include line to compile a
    feature out entirely.  Order: foundation -> surface -> compose / interact / present
    (siblings) -> widgets -> table -> window -> dock -> popup -> nav -> user -> debug -> frame.

    backend/     -- beside the stack, not a rung in it: the render sink (the second TU,
                    gui_backend.c), reached only through gui_backend.h at flush.
    foundation/  -- machinery with no opinions: the ambient context records (s_interaction,
                    s_build, s_scope, g_ctx), identity (ids), keyed state tracking, the io
                    snapshot, and the style machinery (theme registry, stacks, resolution).
                    Owns style MACHINERY, never style MEANING -- no foundation decision reads
                    a style value.
    surface/     -- the surface service: window records as placed, stacked, occluding
                    rectangles -- the pool (window_get/window_find), the next-window
                    placement channel, the z dispenser (surface_z_raise, this tier is its
                    ONLY writer), the hover-win occlusion contest (surface_hover_nominate),
                    the surface reassignment request slot, and open/closed state.  No
                    layout, no chrome, no gestures -- those are window/ policy over these
                    services.  Storage + frame turnover stay with the context
                    (foundation/gui_ctx.c), the house pattern; viewport (OS surface)
                    lifecycle stays with the conductor (app()/rhi() operations).
    compose/     -- composition: the only code that turns style spacing metrics
                    (line_size/gap/pad/quantum/scales) into rects.  Track resolver, regions,
                    children, the public layout verbs + sz_ sizing family.
    interact/    -- behavior: widget-agnostic interaction services -- the standard item
                    protocol (widget_behavior), bare chrome grab (grab_item), drag threshold +
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
    widgets/     -- the stock widget set: prefab emit clients that compose the three sibling
                    roles in one call.  A CLIENT of the tiers below, not a privileged layer.
    window/      -- host structure: persisted window record + window-as-widget chrome.
                    First real optional boundary -- a canvas/HUD-only embedding can skip it.
    popup/       -- host structure, window-dependent overlay stack: popups/tooltips/combo/
                    menus share the open-popup stack (g_ctx->popup.open).
    nav/         -- keyboard nav: a peer service that arbitrates focus across windows, docks,
                    menus, and popups alike -- not a client of any one of them.  Included right
                    after popup/ (dependency order: it reads/drives the popup stack), even
                    though topically it sits beside popup/dock/window, not inside them.
    dock/        -- host structure, window-dependent, independent of popup/: dock-node
                    tree + splitters.
    table/       -- host structure, independent optional feature: needs only the sibling
                    roles and below, no window dependency.
    user/        -- the caller's vocabulary: pure public verbs + readers -- the bracketing
                    stacks (id / item flags / style / scale / disabled), behavior on caller
                    rects (gui_item), the canvas + raw-draw surface, and the query readers
                    (want_capture_*, is_item_*, is_key_*).  Zero state, zero machinery;
                    consumed only from outside the lib (via the vtable) or by lower tiers
                    deliberately dogfooding the public surface through gui_host.h declarations.
                    Where user widgets are written: rect (canvas) + item() + draw_*, no skin.
    debug/       -- dev tooling over the system, not part of it: the pipeline dashboard and
                    the perf/state HUD overlays.  Severable -- a ship build could drop it.
    frame/       -- the conductor: gui_frame.c lifecycle, gui_viewport.c, gui_boot.c.  Top of
                    the stack alongside user/ (the host's driver over the tiers, as user/ is
                    the caller's door into them), NOT a foundation: prepare/update/dispatch
                    touches every tier.
    (root)       -- this unity entry, the public headers, and gui_api.c (vtable, mod_desc).

    Cross-role contract (the composer / behavior / presentation split):
      composition  (compose/)  consumes spacing metrics, produces rects;
      behavior     (interact/) consumes (id, rect), produces interaction state;
      presentation (present/)  consumes rect + state + skin and paints.
    A widget (widgets/ and above) is the only combiner: it asks composition for a rect, hands it to
    behavior, hands both results to presentation.  user/ is the public door onto the same
    roles, skin optional -- the game-UI path.

    foundation/gui_theme.c       -- theme registry + base/active style state, theme API, layout_compute
    foundation/gui_style.c       -- style stacks machinery: style_col/style_var resolution, push/pop/next ops
    foundation/gui_ctx.c         -- context state: s_interaction, s_build, s_scope, layout_frame_t, gui_context_t,
                                      ctx_new_frame, memory stats, multi-context lifecycle (ctx_create/destroy/bind)
    foundation/gui_io.c          -- io snapshot service: app->IO, input_frame_begin/end, s_io
    foundation/gui_id.c          -- identity service: id_hash, id_combine, id_seed/push/pop
    foundation/gui_state.c       -- keyed state tracking service: gui_state_get/peek, GUI_STATE

    surface/gui_surface.c        -- surface service: window record pool, placement channel, z dispenser,
                                      hover-win contest, surface reassignment slot, open/closed state

    compose/gui_layout_core.c    -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    compose/gui_scroll.c  -- scrollable region engine: gui_region_t, gutters, push/pop_region
    compose/gui_layout_child.c   -- child box lifecycle: child_begin/child_end
    compose/gui_sublayout.c      -- transient sub-layout lifecycle: push/pop_layout, sublayout_open
    compose/gui_split.c          -- side-by-side split panels: split_begin/next/end
    compose/gui_region.c         -- root-level region: a fixed-rect layout primitive, no window chrome
    compose/gui_layout.c         -- public layout API verbs + sz_ sizing: gui_layout, gui_stack, gui_cols

    interact/gui_item.c          -- the standard item protocol: widget_behavior, grab_item, nav registration, repeat
    interact/gui_drag.c          -- drag service: threshold machine + typed payload transfer (source/target)
    interact/gui_move.c          -- move-drag protocol (move_grab/move_track) + deferred-press latch (press_defer_*)
    interact/gui_resize.c        -- edge-resize mechanism: resize_item protocol, hit-test, grab, edge apply
    interact/gui_anim.c          -- animation stepping service: gui_anim_f32

    present/gui_paint_core.c    -- shared presentation primitives: COL_* palette, layout macros, label
                                      grammar, system adornments (nav/drop rings, resize highlight)
    present/gui_symbol.c         -- symbol + shape draw primitives: draw_arrow/check/frame/round_rect/arc/...

    widgets/gui_text_edit.c      -- single-line text editing engine: input_field_edit (behind input_text)
    widgets/gui_scrollbar.c      -- scrollbar widget: track + knob over a region-handed rect + scroll slot
    widgets/gui_text.c           -- display widgets: text runs, bullets, label_text, progress_bar, spacers
    widgets/gui_button.c         -- press widgets: button family, checkbox, radio_button, selectable
    widgets/gui_tree.c           -- folding widgets: collapsing_header, tree_node/tree_pop
    widgets/gui_input.c          -- single-line text fields: input_text / _ex / _with_hint
    widgets/gui_volatile.c       -- volatile widgets: per-frame retessellated text/plots (tess_gen slots)
    widgets/gui_widget_slider.c  -- slider + drag widgets: slider_float/int, drag_int, slider_render
    widgets/gui_widget_numeric.c -- numeric text inputs: input_int/float/double, input_float2/3/4

    table/gui_table.c            -- table layout: multi-column rows, self-fitting cells, one table clip (needs only the sibling roles and below)

    window/gui_window.c          -- window gesture policy state: drag mode, merge-back latch, raise-on-press
    window/gui_window_native.c   -- native-borderless windows: identity test, caption buttons, OS-frame sync
    window/gui_window_docked.c   -- the docked branch of window_begin: placed + chromed by its dock node
    window/gui_window_free.c   -- the free-float window: geometry + gesture resolution (window_begin_ex)
    window/gui_window_end.c      -- deferred window chrome: titlebar, buttons, border, resize grip, move grab

    dock/gui_dock_core.c         -- docking: node pool, per-frame layout, splitter interaction + chrome
    dock/gui_dock_float.c        -- floating tab groups: windows tabbed onto one free frame, no splits
    dock/gui_dock_drag.c         -- docking: mouse drag-to-dock / undock-by-tab-drag + tab-strip chrome
    dock/gui_dock.c              -- docking: public build API (dockspace_over_viewport, dock_split, ...)
    dock/gui_dock_serialize.c    -- docking: layout save/load (text blob)
    dock/gui_dock_route.c        -- the window <-> dock route seam: the five verbs window/ may call

    popup/gui_popup.c            -- popups / context menus / tooltips: overlay windows on a reserved z-band
    popup/gui_combo.c     -- combo box + list box: a popup dropdown / a scrolling child of selectables
    popup/gui_menu.c      -- menu bar + menu items: built directly on the popup internals

    nav/gui_nav.c                -- keyboard nav cursor + menu-bar mode (reads/drives the popup stack)

    user/gui_stacks.c            -- bracketing vocabulary: push/pop id, item flags, style color/var, scale, disabled
    user/gui_behavior.c          -- public behavior on caller rects: gui_item, invisible_button
    user/gui_canvas.c            -- custom-draw surface: canvas, draw_rect/text, text measure, icons
    user/gui_query.c             -- public readers: want_capture_*, is_item_*, is_key_*, is_mouse_*, get_mouse_pos

    debug/gui_dashboard.c        -- pipeline dashboard: debug-band window over the backend capture snapshot
    debug/gui_frame_overlay.c    -- built-in perf / state HUD overlays + the frame-timing helpers they read

    frame/gui_frame.c            -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, font, clip
    frame/gui_viewport.c         -- viewport open/resize/close + gui-owned floater lifecycle (spawn/update/render_floaters)
    frame/gui_boot.c             -- one-call host front end: boot, frame_poll, present_begin/present

    gui_api.c                    -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h> /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>   /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

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

static gui_forward_caps_t s_fwd_caps = { .tables = true, .docking = true, .keyboard_nav = true };

/* The theme registry, base/active style state (s_style_base, s_style, s_font_size), the theme
   API, and layout_compute now live in foundation/gui_theme.c -- included first among foundation/ below, so
   s_style is declared before foundation/gui_style.c's push-stack resolvers (and every later tier) read
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

// Foundations interleaved with the sibling roles (present/ primitives, interact/
// services): the include order is dependency-driven, the directories are the conceptual split.
#include "runtime_service/gui/foundation/gui_theme.c"
#include "runtime_service/gui/foundation/gui_io.c"
#include "runtime_service/gui/foundation/gui_style.c"
#include "runtime_service/gui/foundation/gui_ctx.c"
#include "runtime_service/gui/foundation/gui_id.c"
#include "runtime_service/gui/foundation/gui_state.c"

#include "runtime_service/gui/surface/gui_surface.c"

#include "runtime_service/gui/present/gui_paint_core.c"
#include "runtime_service/gui/interact/gui_item.c"
#include "runtime_service/gui/interact/gui_drag.c"
#include "runtime_service/gui/interact/gui_move.c"
#include "runtime_service/gui/present/gui_symbol.c"
#include "runtime_service/gui/interact/gui_resize.c"

// Composition (spacing metrics in, rects out)
#include "runtime_service/gui/compose/gui_layout_core.c"
#include "runtime_service/gui/compose/gui_scroll.c"
#include "runtime_service/gui/compose/gui_layout_child.c"
#include "runtime_service/gui/compose/gui_sublayout.c"
#include "runtime_service/gui/compose/gui_split.c"
#include "runtime_service/gui/compose/gui_region.c"
#include "runtime_service/gui/compose/gui_layout.c"

#include "runtime_service/gui/interact/gui_anim.c"

// The stock widget set (a client of the tiers above).  Internal
// calls to the user/ vocabulary (gui_canvas tooltips, combo's push_id) resolve through the public
// declarations in gui_host.h -- deliberate dogfooding of the caller surface, not an order cycle.
#include "runtime_service/gui/widgets/gui_text_edit.c"
#include "runtime_service/gui/widgets/gui_scrollbar.c"
#include "runtime_service/gui/widgets/gui_text.c"
#include "runtime_service/gui/widgets/gui_button.c"
#include "runtime_service/gui/widgets/gui_tree.c"
#include "runtime_service/gui/widgets/gui_input.c"
#include "runtime_service/gui/widgets/gui_volatile.c"
#include "runtime_service/gui/widgets/gui_widget_slider.c"
#include "runtime_service/gui/widgets/gui_widget_numeric.c"

// Table -- independent optional feature (needs only the sibling roles and below, no window dependency)
#include "runtime_service/gui/table/gui_table.c"

// Window subsystem (first real optional boundary)
#include "runtime_service/gui/window/gui_window.c"
#include "runtime_service/gui/window/gui_window_native.c"
#include "runtime_service/gui/window/gui_window_docked.c"
#include "runtime_service/gui/window/gui_window_free.c"
#include "runtime_service/gui/window/gui_window_end.c"

// Dock -- window-dependent, independent of popup/
#include "runtime_service/gui/dock/gui_dock_core.c"
#include "runtime_service/gui/dock/gui_dock_float.c"
#include "runtime_service/gui/dock/gui_dock_drag.c"
#include "runtime_service/gui/dock/gui_dock.c"
#include "runtime_service/gui/dock/gui_dock_serialize.c"
#include "runtime_service/gui/dock/gui_dock_route.c"

// Popup -- window-dependent overlay stack (popup/combo/menu share g_ctx->popup.open)
#include "runtime_service/gui/popup/gui_popup.c"
// Nav -- keyboard nav: a peer service over windows/docks/menus/popups alike, included here
// (not with popup/) only because it reads/drives the popup stack gui_popup.c just opened.
#include "runtime_service/gui/nav/gui_nav.c"
#include "runtime_service/gui/popup/gui_combo.c"
#include "runtime_service/gui/popup/gui_menu.c"

// user/ -- the caller's vocabulary.  Pure public verbs + readers over the machinery
// above; zero state, zero machinery -- deleting any file here breaks no lower tier.  Included
// last-of-the-tiers because nothing below needs them at definition time (all upward calls go
// through gui_host.h declarations).
#include "runtime_service/gui/user/gui_stacks.c"
#include "runtime_service/gui/user/gui_behavior.c"
#include "runtime_service/gui/user/gui_canvas.c"
#include "runtime_service/gui/user/gui_query.c"

// Pipeline dashboard -- an ordinary debug-band window + panel painters over the standard draw
// API; the snapshot it reads is captured in the backend unit (backend/gui_dash_capture.c).
#include "runtime_service/gui/debug/gui_dashboard.c"

// Command stepper -- an ordinary debug-band window controlling the frozen-frame replay; the
// capture + restore live in the backend unit (backend/gui_step_capture.c).
#include "runtime_service/gui/debug/gui_step_window.c"

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

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
// clang-format on