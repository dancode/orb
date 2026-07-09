/*==============================================================================================

    runtime_service/gui/gui.c -- Unity build entry for the gui UI / core unit.

    gui is two translation units linked into one static lib (see gui_backend.h):

      - this unit (gui.c): context, layout, widgets, chrome, popups, nav, input, frame
        lifecycle, the module vtable.  Owns s_build / s_scope / s_io / s_interaction / g_ctx
        and the stacks.

      - the render backend (gui_backend.c): fonts, draw list, tessellation, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_font / s_render.  Called through gui_backend.h.
    
    Include order matters: each file can reference statics from files included above it.  Files
    live in NUMBERED TIER directories -- the directory listing IS the stack, bottom-up.  A tier
    only depends on tiers below it, so a numbered dir is also where a future
    `#ifdef GUI_ENABLE_<tier>` would wrap an #include line to compile a feature out entirely.
    The include order below is dependency-driven and interleaves within a tier; the directories
    are the conceptual split.

    backend/        -- beside the stack, not a rung in it: the render sink (the second TU,
                       gui_backend.c), reached only through gui_backend.h at flush.
    0_foundation/   -- machinery with no opinions: the ambient context records (s_interaction,
                       s_build, s_scope, g_ctx), identity (ids), keyed state tracking, the io
                       snapshot, and the style machinery (theme registry, stacks, resolution).
                       Owns style MACHINERY, never style MEANING -- no foundation decision reads
                       a style value.
    1_surface/      -- the surface service: window records as placed, stacked, occluding
                       rectangles -- the pool (window_get/window_find), the next-window
                       placement channel, the z dispenser (surface_z_raise, this tier is its
                       ONLY writer), the hover-win occlusion contest (surface_hover_nominate),
                       the surface reassignment request slot, and open/closed state.  No
                       layout, no chrome, no gestures -- those are 4_window/ policy over these
                       services.  Storage + frame turnover stay with the context
                       (0_foundation/gui_ctx.c), the house pattern; viewport (OS surface)
                       lifecycle stays with the conductor (app()/rhi() operations).
    2_compose/      -- composition: the only code that turns style spacing metrics
                       (line_size/gap/pad/quantum/scales) into rects.  Track resolver, regions,
                       children, the public layout verbs + sz_ sizing family.
    2_interact/     -- behavior: widget-agnostic interaction services -- the standard item
                       protocol (widget_behavior), bare chrome grab (grab_item), drag threshold +
                       payload, move-drag + deferred-press (move_grab/move_track, press_defer_*),
                       edge-resize mechanism (resize_item), animation stepping.  Each serves a
                       capability (exclusivity, clicks, tracking) over (id, rect); none knows a
                       widget, and none reads a style value or paints -- system adornments (nav
                       ring, drop ring, hot edges) are invoked from here but painted by 2_present/
                       helpers.  This tier is the ONLY writer of the s_interaction arbitration
                       fields (hover/active/focus): higher tiers claim through these verbs and
                       read the record for gating, never write it raw (one exception: the popup
                       modal hover fence, gui_popup.c).  Behavior's only inputs beyond (id, rect)
                       are the interaction scope (s_scope: owner window, clip, chrome
                       suppression, per-item flag/nav stamps) -- stamped by composition at its
                       seams -- and its own s_interaction; it never reads the composer scratch
                       (s_build).
    2_present/      -- presentation: the shared paint primitives -- COL_* palette, widget
                       macros, label grammar, text-fit, symbol/shape draws.  Consumes
                       rect + state + skin; never asks behavior, state is a parameter.
                       The three 2_* dirs are SIBLINGS -- combined only by the tiers above.
    3_widgets/      -- the stock widget set: prefab emit clients that compose the three tier-2
                       roles in one call.  A CLIENT of the tiers below, not a privileged layer.
    4_window/       -- host structure: persisted window record + window-as-widget chrome.
                       First real optional boundary -- a canvas/HUD-only embedding can skip it.
    4_popup/        -- host structure, window-dependent overlay stack: popups/tooltips/combo/
                       menus/nav all share the open-popup stack (g_ctx->popups_open).
    4_dock/         -- host structure, window-dependent, independent of 4_popup/: dock-node
                       tree + splitters.
    4_table/        -- host structure, independent optional feature: needs tiers 0-2 only,
                       no window dependency.
    5_user/         -- the caller's vocabulary: pure public verbs + readers -- the bracketing
                       stacks (id / item flags / style / scale / disabled), behavior on caller
                       rects (gui_item), the canvas + raw-draw surface, and the query readers
                       (want_capture_*, is_item_*, is_key_*).  Zero state, zero machinery;
                       consumed only from outside the lib (via the vtable) or by lower tiers
                       deliberately dogfooding the public surface through gui_host.h declarations.
                       Where user widgets are written: rect (canvas) + item() + draw_*, no skin.
    (root)          -- the conductor: gui_frame.c lifecycle, gui_boot.c, the dashboard +
                       overlays, gui_api.c vtable.  Top of the stack alongside 5_user/ (the
                       host's driver over the tiers, as user/ is the caller's door into them),
                       NOT a foundation: prepare/update/dispatch touches every tier.

    Cross-tier contract (the composer / behavior / presentation split, tier 2):
      composition  (2_compose/)  consumes spacing metrics, produces rects;
      behavior     (2_interact/) consumes (id, rect), produces interaction state;
      presentation (2_present/)  consumes rect + state + skin and paints.
    A widget (tier 3+) is the only combiner: it asks composition for a rect, hands it to
    behavior, hands both results to presentation.  5_user/ is the public door onto the same
    roles, skin optional -- the game-UI path.

    0_foundation/gui_theme.c       -- theme registry + base/active style state, theme API, layout_compute
    0_foundation/gui_style.c       -- style stacks machinery: style_col/style_var resolution, push/pop/next ops
    0_foundation/gui_ctx.c         -- context state: s_interaction, s_build, s_scope, layout_frame_t, gui_context_t, ctx_new_frame
    0_foundation/gui_io.c          -- io snapshot service: app->IO, input_update, s_io
    0_foundation/gui_id.c          -- identity service: id_hash, id_combine, id_seed/push/pop
    0_foundation/gui_state.c       -- keyed state tracking service: gui_state_get/peek, GUI_STATE

    1_surface/gui_surface.c        -- surface service: window record pool, placement channel, z dispenser,
                                      hover-win contest, surface reassignment slot, open/closed state

    2_compose/gui_layout_core.c    -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    2_compose/gui_layout_region.c  -- scrollable region engine: gui_region_t, gutters, push/pop_region
    2_compose/gui_layout_child.c   -- child box + sub-layout lifecycle: begin/child_end, push/pop_layout
    2_compose/gui_region.c         -- root-level region: a fixed-rect layout primitive, no window chrome
    2_compose/gui_layout.c         -- public layout API verbs + sz_ sizing: gui_layout, gui_stack, gui_cols

    2_interact/gui_item.c          -- the standard item protocol: widget_behavior, grab_item, nav registration, repeat
    2_interact/gui_drag.c          -- drag service: threshold machine + typed payload transfer (source/target)
    2_interact/gui_move.c          -- move-drag protocol (move_grab/move_track) + deferred-press latch (press_defer_*)
    2_interact/gui_resize.c        -- edge-resize mechanism: resize_item protocol, hit-test, grab, edge apply
    2_interact/gui_anim.c          -- animation stepping service: gui_anim_f32

    2_present/gui_widget_core.c    -- shared presentation primitives: COL_* palette, layout macros, label
                                      grammar, system adornments (nav/drop rings, resize highlight)
    2_present/gui_symbol.c         -- symbol + shape draw primitives: draw_arrow/check/frame/round_rect/arc/...

    3_widgets/gui_text_edit.c      -- single-line text editing engine: input_field_edit (behind input_text)
    3_widgets/gui_scrollbar.c      -- scrollbar widget: track + knob over a region-handed rect + scroll slot
    3_widgets/gui_widget.c         -- core leaf widgets: text, button, checkbox, input_text, selectable
    3_widgets/gui_volatile.c       -- volatile widgets: per-frame retessellated text/plots (tess_gen slots)
    3_widgets/gui_widget_slider.c  -- slider + drag widgets: slider_float/int, drag_int, slider_render
    3_widgets/gui_widget_numeric.c -- numeric text inputs: input_int/float/double, input_float2/3/4

    4_table/gui_table.c            -- table layout: multi-column rows, self-fitting cells, one table clip (needs tiers 0-2 only)

    4_window/gui_window.c          -- window gesture policy state: drag mode, merge-back latch, raise-on-press
    4_window/gui_window_native.c   -- native-borderless windows: identity test, caption buttons, OS-frame sync
    4_window/gui_widget_window.c   -- the window as a widget: begin/window_end + chrome (resize); body is a region

    4_dock/gui_dock_core.c         -- docking: node pool, per-frame layout, splitter interaction + chrome
    4_dock/gui_dock_float.c        -- floating tab groups: windows tabbed onto one free frame, no splits
    4_dock/gui_dock_drag.c         -- docking: mouse drag-to-dock / undock-by-tab-drag + tab-strip chrome
    4_dock/gui_dock.c              -- docking: public build API (dockspace_over_viewport, dock_split, ...)
    4_dock/gui_dock_serialize.c    -- docking: layout save/load (text blob)

    4_popup/gui_popup.c            -- popups / context menus / tooltips: overlay windows on a reserved z-band
    4_popup/gui_nav.c              -- keyboard nav cursor + menu-bar mode (reads/drives the popup stack)
    4_popup/gui_widget_combo.c     -- combo box + list box: a popup dropdown / a scrolling child of selectables
    4_popup/gui_widget_menu.c      -- menu bar + menu items: built directly on the popup internals

    5_user/gui_stacks.c            -- bracketing vocabulary: push/pop id, item flags, style color/var, scale, disabled
    5_user/gui_behavior.c          -- public behavior on caller rects: gui_item, invisible_button
    5_user/gui_canvas.c            -- custom-draw surface: canvas, draw_rect/text, text measure, icons
    5_user/gui_query.c             -- public readers: want_capture_*, is_item_*, is_key_*, is_mouse_*, get_mouse_pos

    gui_dashboard.c                -- pipeline dashboard: debug-band window over the backend capture snapshot
    gui_frame_overlay.c            -- built-in perf / state HUD overlays + the frame-timing helpers they read
    gui_frame.c                    -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, viewport, font, clip
    gui_boot.c                     -- one-call host front end: boot, frame_poll, present_begin/present
    gui_api.c                      -- vtable, mod_desc, MOD_DEFINE_EXPORTS

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
   API, and layout_compute now live in 0_foundation/gui_theme.c -- included first among Tier 0 below, so
   s_style is declared before 0_foundation/gui_style.c's push-stack resolvers (and every later tier) read
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

// Tier 0 foundations interleaved with the tier-2 roles (2_present/ primitives, 2_interact/
// services): the include order is dependency-driven, the directories are the conceptual split.
#include "runtime_service/gui/0_foundation/gui_theme.c"
#include "runtime_service/gui/0_foundation/gui_io.c"
#include "runtime_service/gui/0_foundation/gui_style.c"
#include "runtime_service/gui/0_foundation/gui_ctx.c"
#include "runtime_service/gui/0_foundation/gui_id.c"
#include "runtime_service/gui/0_foundation/gui_state.c"

#include "runtime_service/gui/1_surface/gui_surface.c"

#include "runtime_service/gui/2_present/gui_widget_core.c"
#include "runtime_service/gui/2_interact/gui_item.c"
#include "runtime_service/gui/2_interact/gui_drag.c"
#include "runtime_service/gui/2_interact/gui_move.c"
#include "runtime_service/gui/2_present/gui_symbol.c"
#include "runtime_service/gui/2_interact/gui_resize.c"

// Tier 2 -- composition (spacing metrics in, rects out)
#include "runtime_service/gui/2_compose/gui_layout_core.c"
#include "runtime_service/gui/2_compose/gui_layout_region.c"
#include "runtime_service/gui/2_compose/gui_layout_child.c"
#include "runtime_service/gui/2_compose/gui_region.c"
#include "runtime_service/gui/2_compose/gui_layout.c"

#include "runtime_service/gui/2_interact/gui_anim.c"

// Tier 3 -- the stock widget set (a client of the tiers above).  Internal
// calls to the 5_user/ vocabulary (gui_canvas tooltips, combo's push_id) resolve through the public
// declarations in gui_host.h -- deliberate dogfooding of the caller surface, not an order cycle.
#include "runtime_service/gui/3_widgets/gui_text_edit.c"
#include "runtime_service/gui/3_widgets/gui_scrollbar.c"
#include "runtime_service/gui/3_widgets/gui_widget.c"
#include "runtime_service/gui/3_widgets/gui_volatile.c"
#include "runtime_service/gui/3_widgets/gui_widget_slider.c"
#include "runtime_service/gui/3_widgets/gui_widget_numeric.c"

// Tier 4 -- independent optional feature (needs tiers 0-2 only, no window dependency)
#include "runtime_service/gui/4_table/gui_table.c"

// Tier 4 -- window subsystem (first real optional boundary; holds the future 1_surface/ record)
#include "runtime_service/gui/4_window/gui_window.c"
#include "runtime_service/gui/4_window/gui_window_native.c"
#include "runtime_service/gui/4_window/gui_widget_window.c"

// Tier 4 -- window-dependent, independent of 4_popup/
#include "runtime_service/gui/4_dock/gui_dock_core.c"
#include "runtime_service/gui/4_dock/gui_dock_float.c"
#include "runtime_service/gui/4_dock/gui_dock_drag.c"
#include "runtime_service/gui/4_dock/gui_dock.c"
#include "runtime_service/gui/4_dock/gui_dock_serialize.c"

// Tier 4 -- window-dependent overlay stack (popup/nav/combo/menu share g_ctx->popups_open)
#include "runtime_service/gui/4_popup/gui_popup.c"
#include "runtime_service/gui/4_popup/gui_nav.c"
#include "runtime_service/gui/4_popup/gui_widget_combo.c"
#include "runtime_service/gui/4_popup/gui_widget_menu.c"

// Tier 5 -- 5_user/: the caller's vocabulary.  Pure public verbs + readers over the machinery
// above; zero state, zero machinery -- deleting any file here breaks no lower tier.  Included
// last-of-the-tiers because nothing below needs them at definition time (all upward calls go
// through gui_host.h declarations).
#include "runtime_service/gui/5_user/gui_stacks.c"
#include "runtime_service/gui/5_user/gui_behavior.c"
#include "runtime_service/gui/5_user/gui_canvas.c"
#include "runtime_service/gui/5_user/gui_query.c"

// Pipeline dashboard -- an ordinary debug-band window + panel painters over the standard draw
// API; the snapshot it reads is captured in the backend unit (backend/gui_dash_capture.c).
#include "runtime_service/gui/gui_dashboard.c"

// Orchestration -- sits above every tier, drives whichever are compiled in.  The overlay file
// carries the perf/state HUDs plus the frame-timing helpers the lifecycle in gui_frame.c calls,
// so it must precede gui_frame.c in the unity build.
#include "runtime_service/gui/gui_frame_overlay.c"
#include "runtime_service/gui/gui_frame.c"

// Boot-tier host front end -- one-call setup (boot) + the canonical loop (frame_poll,
// present_begin/present).  Last: it composes the lifecycle, viewport, and window layers above.
#include "runtime_service/gui/gui_boot.c"

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
// clang-format on