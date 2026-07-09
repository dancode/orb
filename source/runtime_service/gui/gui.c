/*==============================================================================================

    runtime_service/gui/gui.c -- Unity build entry for the gui UI / core unit.

    gui is two translation units linked into one static lib (see gui_backend.h):

      - this unit (gui.c): context, layout, widgets, chrome, popups, nav, input, frame
        lifecycle, the module vtable.  Owns s_build / s_io / s_interaction / g_ctx and the stacks.

      - the render backend (gui_backend.c): fonts, draw list, tessellation, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_font / s_render.  Called through gui_backend.h.
    
    Include order matters: each file can reference statics from files included above it.  Files
    are grouped into subdirectories by DEPENDENCY TIER, not by theme -- each tier only needs the
    tiers above it, so this is also where a future `#ifdef GUI_ENABLE_<tier>` would wrap an
    #include line to compile a feature out entirely.

    core/        -- Tier 0a, the context + style machinery: the ambient state records
                    (s_interaction, s_build, g_ctx), style resolution (theme/style/stacks),
                    and the shared presentation primitives (macros, palette, label grammar).
    interact/    -- Tier 0a, the interaction services: widget-agnostic utilities the higher
                    tiers compose behavior from -- identity (ids), keyed state tracking, the
                    io snapshot, the standard item protocol (widget_behavior), drag payloads,
                    edge-resize mechanism, animation stepping.  Each serves a capability
                    (exclusivity, clicks, tracking) over (id, rect); none knows a widget.
                    A service consumes finished RECTS -- it never reads a style metric to
                    make a decision; style is invisible to this tier (the nav ring is the one
                    sanctioned exception, see interact/gui_item.c).
    compose/     -- Tier 0b, the layout composer: the only code that turns style spacing
                    metrics (line_size/gap/pad/quantum/scales) into rects.  Track resolver,
                    regions, children, the public layout verbs + sz_ sizing family.
    widgets/     -- Tier 1, the stock presentation layer: the default widget set, painting
                    skin + inner geometry inside composer rects via the item protocol.  The
                    stock set is a CLIENT of the tiers above it, not a privileged layer.
    window/      -- Tier 2, window subsystem: persisted window record + window-as-widget chrome.
                    First real optional boundary -- a canvas/HUD-only embedding can skip it.
    popup/       -- Tier 3, window-dependent overlay stack: popups/tooltips/combo/menus/nav all
                    share the open-popup stack (s_popups_open), so nav and menus live here too.
    dock/        -- Tier 3, window-dependent, independent of popup/: dock-node tree + splitters.
    table/       -- Tier 4, independent optional feature: needs only core/+compose/, no window
                    dependency.
    user/        -- Tier 5, the caller's vocabulary: pure public verbs + readers -- the
                    bracketing stacks (id / item flags / style / scale / disabled), behavior on
                    caller rects (gui_item), the canvas + raw-draw surface, and the query
                    readers (want_capture_*, is_item_*, is_key_*).  Zero state, zero machinery;
                    consumed only from outside the lib (via the vtable) or by lower tiers
                    deliberately dogfooding the public surface through gui_host.h declarations.
                    Where user widgets are written: rect (canvas) + item() + draw_*, no skin.

    Cross-tier contract (the composer / behavior / presentation split):
      composer (compose/) consumes spacing metrics, produces rects;
      behavior (interact/) consumes rects, produces interaction state;
      presentation (widgets/ + chrome) consumes rect + state + skin and paints.
    user/ is the public door onto the first two, skin optional -- the game-UI path.

    core/gui_theme.c            -- theme registry + base/active style state, theme API, layout_compute
    core/gui_style.c            -- style stacks machinery: style_col/style_var resolution, push/pop/next ops
    core/gui_ctx.c              -- context state: s_interaction, s_build, layout_frame_t, gui_context_t, ctx_new_frame
    core/gui_widget_core.c      -- shared presentation primitives: COL_* palette, layout macros, label grammar
    core/gui_symbol.c           -- symbol + shape draw primitives: draw_arrow/check/frame/round_rect/arc/...

    interact/gui_io.c           -- io snapshot service: app->IO, input_update, s_io
    interact/gui_id.c           -- identity service: id_hash, id_combine, id_seed/push/pop
    interact/gui_state.c        -- keyed state tracking service: gui_state_get/peek, GUI_STATE
    interact/gui_item.c         -- the standard item protocol: widget_behavior, nav registration, repeat
    interact/gui_drag.c         -- drag service: threshold machine + typed payload transfer (source/target)
    interact/gui_resize.c       -- edge-resize mechanism: hit-test, highlight, grab, edge apply
    interact/gui_anim.c         -- animation stepping service: gui_anim_f32, gui_anim_bg

    compose/gui_layout_core.c   -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    compose/gui_layout_region.c -- scrollable region engine: gui_region_t, region_scrollbar, push/pop_region
    compose/gui_layout_child.c  -- child box + sub-layout lifecycle: begin/child_end, push/pop_layout
    compose/gui_region.c        -- root-level region: a fixed-rect layout primitive, no window chrome
    compose/gui_layout.c        -- public layout API verbs + sz_ sizing: gui_layout, gui_stack, gui_cols

    user/gui_stacks.c            -- bracketing vocabulary: push/pop id, item flags, style color/var, scale, disabled
    user/gui_behavior.c          -- public behavior on caller rects: gui_item, invisible_button
    user/gui_canvas.c            -- custom-draw surface: canvas, draw_rect/text, text measure, icons
    user/gui_query.c             -- public readers: want_capture_*, is_item_*, is_key_*, is_mouse_*, get_mouse_pos

    widgets/gui_text_edit.c      -- single-line text editing engine: input_field_edit (behind input_text)
    widgets/gui_widget.c         -- core leaf widgets: text, button, checkbox, input_text, selectable
    widgets/gui_widget_slider.c  -- slider + drag widgets: slider_float/int, drag_int, slider_render
    widgets/gui_widget_numeric.c -- numeric text inputs: input_int/float/double, input_float2/3/4

    table/gui_table.c            -- table layout: multi-column rows, self-fitting cells, one table clip (needs core/ only)

    window/gui_window.c          -- persistent per-window state: gui_window_t, window_get, drag mode
    window/gui_widget_window.c   -- the window as a widget: begin/window_end + chrome (resize); body is a region

    dock/gui_dock_core.c         -- docking: node pool, per-frame layout, splitter interaction + chrome
    dock/gui_dock_float.c        -- floating tab groups: windows tabbed onto one free frame, no splits
    dock/gui_dock_drag.c         -- docking: mouse drag-to-dock / undock-by-tab-drag + tab-strip chrome
    dock/gui_dock.c              -- docking: public build API (dockspace_over_viewport, dock_split, ...)
    dock/gui_dock_serialize.c    -- docking: layout save/load (text blob)

    popup/gui_popup.c            -- popups / context menus / tooltips: overlay windows on a reserved z-band
    popup/gui_nav.c              -- keyboard nav cursor + menu-bar mode (reads/drives the popup stack)
    popup/gui_widget_combo.c     -- combo box + list box: a popup dropdown / a scrolling child of selectables
    popup/gui_widget_menu.c      -- menu bar + menu items: built directly on the popup internals

    gui_frame_overlay.c         -- built-in perf / state HUD overlays + the frame-timing helpers they read
    gui_frame.c                 -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, viewport, font, clip
    gui_api.c                   -- vtable, mod_desc, MOD_DEFINE_EXPORTS

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
   API, and layout_compute now live in core/gui_theme.c -- included first among Tier 0 below, so
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

// Tier 0a -- core (context + style machinery) interleaved with interact/ (the interaction
// services): the include order is dependency-driven, the directories are the conceptual split.
#include "runtime_service/gui/core/gui_theme.c"
#include "runtime_service/gui/interact/gui_io.c"
#include "runtime_service/gui/core/gui_style.c"
#include "runtime_service/gui/core/gui_ctx.c"
#include "runtime_service/gui/interact/gui_id.c"
#include "runtime_service/gui/interact/gui_state.c"
#include "runtime_service/gui/core/gui_widget_core.c"
#include "runtime_service/gui/interact/gui_item.c"
#include "runtime_service/gui/interact/gui_drag.c"
#include "runtime_service/gui/core/gui_symbol.c"
#include "runtime_service/gui/interact/gui_resize.c"

// Tier 0b -- the layout composer (spacing metrics in, rects out)
#include "runtime_service/gui/compose/gui_layout_core.c"
#include "runtime_service/gui/compose/gui_layout_region.c"
#include "runtime_service/gui/compose/gui_layout_child.c"
#include "runtime_service/gui/compose/gui_region.c"
#include "runtime_service/gui/compose/gui_layout.c"

#include "runtime_service/gui/interact/gui_anim.c"

// Tier 1 -- stock presentation: the default widget set (a client of the tiers above).  Internal
// calls to the user/ vocabulary (gui_canvas tooltips, combo's push_id) resolve through the public
// declarations in gui_host.h -- deliberate dogfooding of the caller surface, not an order cycle.
#include "runtime_service/gui/widgets/gui_text_edit.c"
#include "runtime_service/gui/widgets/gui_widget.c"
#include "runtime_service/gui/widgets/gui_volatile.c"
#include "runtime_service/gui/widgets/gui_widget_slider.c"
#include "runtime_service/gui/widgets/gui_widget_numeric.c"

// Tier 4 -- independent optional feature (needs core/ + compose/ only, no window dependency)
#include "runtime_service/gui/table/gui_table.c"

// Tier 2 -- window subsystem (first real optional boundary)
#include "runtime_service/gui/window/gui_window.c"
#include "runtime_service/gui/window/gui_widget_window.c"

// Tier 3 -- window-dependent, independent of popup/
#include "runtime_service/gui/dock/gui_dock_core.c"
#include "runtime_service/gui/dock/gui_dock_float.c"
#include "runtime_service/gui/dock/gui_dock_drag.c"
#include "runtime_service/gui/dock/gui_dock.c"
#include "runtime_service/gui/dock/gui_dock_serialize.c"

// Tier 3 -- window-dependent overlay stack (popup/nav/combo/menu share s_popups_open)
#include "runtime_service/gui/popup/gui_popup.c"
#include "runtime_service/gui/popup/gui_nav.c"
#include "runtime_service/gui/popup/gui_widget_combo.c"
#include "runtime_service/gui/popup/gui_widget_menu.c"

// Tier 5 -- user/: the caller's vocabulary.  Pure public verbs + readers over the machinery
// above; zero state, zero machinery -- deleting any file here breaks no lower tier.  Included
// last-of-the-tiers because nothing below needs them at definition time (all upward calls go
// through gui_host.h declarations).
#include "runtime_service/gui/user/gui_stacks.c"
#include "runtime_service/gui/user/gui_behavior.c"
#include "runtime_service/gui/user/gui_canvas.c"
#include "runtime_service/gui/user/gui_query.c"

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