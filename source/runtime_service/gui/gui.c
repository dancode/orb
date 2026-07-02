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

    core/        -- Tier 0, foundation: everything below needs this, it needs nothing else in gui.
    widgets/     -- Tier 1, leaf widgets: built on core/ only.
    window/      -- Tier 2, window subsystem: persisted window record + window-as-widget chrome.
                    First real optional boundary -- a canvas/HUD-only embedding can skip it.
    popup/       -- Tier 3, window-dependent overlay stack: popups/tooltips/combo/menus/nav all
                    share the open-popup stack (s_popups_open), so nav and menus live here too.
    dock/        -- Tier 3, window-dependent, independent of popup/: dock-node tree + splitters.
    table/       -- Tier 4, independent optional feature: needs only core/, no window dependency.

    core/gui_input.c         -- app->IO snapshot: input_update, s_io
    core/gui_style.c         -- style stacks: colors + metrics, style_col/style_var, push/pop/next
    core/gui_ctx.c           -- context state: s_interaction, s_build, layout_frame_t, gui_context_t, ctx_new_frame
    core/gui_ctx_id.c        -- id system + state pool: id_hash, id_combine, id_seed/push/pop, gui_state_get
    core/gui_ctx_io.c        -- public IO accessors: want_capture_*, is_key_*, is_mouse_*, get_mouse_pos
    core/gui_widget_core.c   -- shared widget primitives + theme: widget_behavior, COL_*, layout macros
    core/gui_stacks.c        -- push-model public API: push/pop id, item flags, style color / var
    core/gui_symbol.c        -- symbol + shape draw primitives: draw_arrow/check/frame/round_rect/arc/...
    core/gui_resize.c        -- shared edge-resize geometry: hit-test, highlight, grab, edge apply
    core/gui_layout_core.c   -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    core/gui_layout_region.c -- scrollable region engine: gui_region_t, region_scrollbar, push/pop_region
    core/gui_layout_child.c  -- child box + sub-layout lifecycle: begin/child_end, push/pop_layout
    core/gui_layout.c        -- public layout API verbs: gui_layout, gui_stack, gui_cols, gui_grid
    core/gui_anim.c          -- widget animation utilities: gui_anim_f32, gui_anim_bg

    widgets/gui_text_edit.c      -- single-line text editing engine: input_field_edit (behind input_text)
    widgets/gui_widget.c         -- core leaf widgets: text, button, checkbox, input_text, selectable
    widgets/gui_widget_slider.c  -- slider + drag widgets: slider_float/int, drag_int, slider_render
    widgets/gui_widget_numeric.c -- numeric text inputs: input_int/float/double, input_float2/3/4

    table/gui_table.c            -- table layout: multi-column rows with cell clipping (needs core/ only)

    window/gui_window.c          -- persistent per-window state: gui_window_t, window_get, drag mode
    window/gui_widget_window.c   -- the window as a widget: begin/window_end + chrome (resize); body is a region

    dock/gui_dock.c              -- docking: dock-node tree, splitters, tab strips, dockspace + build API

    popup/gui_popup.c            -- popups / context menus / tooltips: overlay windows on a reserved z-band
    popup/gui_nav.c              -- keyboard nav cursor + menu-bar mode (reads/drives the popup stack)
    popup/gui_widget_combo.c     -- combo box + list box: a popup dropdown / a scrolling child of selectables
    popup/gui_widget_menu.c      -- menu bar + menu items: built directly on the popup internals

    gui_frame.c              -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, viewport, font, clip
    gui_api.c                -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h> /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>   /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls gui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/gui/gui_backend.h"

// API function headers + access pointers -- wired at module init/reload time
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
    Layout

    All dimensions are integer pixel counts derived from the active font's *type size* (em) --
    not its glyph-box height (char_h = ascent + descent), which runs ~1.3x the em and would
    inflate every padding.  The em is the design unit a typographer reasons in, so spacing,
    padding, and control heights all scale off it and stay proportional across fonts.
    Defaults match a 12px em.
==============================================================================================*/

/* gui_metrics_t (the layout metric record) is defined in gui_internal.h. */

/* Font type size (em) used by layout_compute; updated by font_load(). */
static u32 s_font_size = 0;

/* Built-in theme registry.  Each entry is a complete gui_style_t authored for em=12;
   layout_compute scales the metrics to the active font.  Add more here; the array is const
   so its name pointers remain stable for the lifetime of the process. */
static const gui_theme_t k_themes[] =
{
    {
        "dark",
        {
            .colors = {
                [ GUI_COL_TEXT         ] = GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ),
                [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ),
                [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0x24, 0x24, 0x24, 0xE8 ),
                [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0x1C, 0x1C, 0x1C, 0xFF ),
                [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x10, 0x60, 0xA0, 0xFF ),
                [ GUI_COL_BORDER       ] = GUI_COLOR( 0x80, 0x80, 0x80, 0xFF ),
                [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0x40, 0x40, 0x40, 0xFF ),
                [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x50, 0x80, 0xB0, 0xFF ),
                [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x30, 0x60, 0x90, 0xFF ),
                [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x20, 0x90, 0xD0, 0xFF ),
                [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x18, 0xE6, 0x48, 0xFF ),
                [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0x30, 0x30, 0x30, 0xFF ),
                [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
                [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0x38, 0x38, 0x38, 0xFF ),
                [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0x20, 0x50, 0x70, 0xFF ),
                [ GUI_COL_CURSOR       ] = GUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF ),
                [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
            },
            .line_size     = 20,
            .widget_gap    = 3,
            .widget_pad    = 6,
            .win_title_h   = 23,
            .win_border    = 1,
            .checkbox_sz   = 16,
            .slider_knob_w = 12,
            .min_cell_w    = 40,
            .checkmark_pad = 4,
            .cursor_w      = 1,
            .cursor_inset  = 3,
            .win_rounding    = 6,
            .widget_rounding = 4,
            .grab_rounding   = 4,
            .check_style     = GUI_CHECK_TICK,
            .bullet_style    = GUI_BULLET_DISC,
            .arrow_style     = GUI_ARROW_FILLED,
            .separator_style = GUI_SEPARATOR_SOLID,
            .progress_style  = GUI_PROGRESS_SOLID,
            .slider_knob     = GUI_SLIDER_KNOB_BAR,
            .menu_check      = GUI_MENU_CHECK_BOX,
        },
    },
    {
        "light",
        {
            .colors = {
                [ GUI_COL_TEXT         ] = GUI_COLOR( 0x10, 0x10, 0x10, 0xFF ),
                [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ),
                [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ),
                [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0xE4, 0xE4, 0xE4, 0xFF ),
                [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x20, 0x80, 0xC0, 0xFF ),
                [ GUI_COL_BORDER       ] = GUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ),
                [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0xD0, 0xD0, 0xD0, 0xFF ),
                [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x60, 0xA0, 0xD0, 0xFF ),
                [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x40, 0x80, 0xB0, 0xFF ),
                [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x20, 0x80, 0xC0, 0xFF ),
                [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x10, 0xA0, 0x30, 0xFF ),
                [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0xC0, 0xC0, 0xC0, 0xFF ),
                [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x30, 0x90, 0xE0, 0xFF ),
                [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0xE8, 0xE8, 0xE8, 0xFF ),
                [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0xC0, 0xD8, 0xF0, 0xFF ),
                [ GUI_COL_CURSOR       ] = GUI_COLOR( 0x10, 0x10, 0x60, 0xFF ),
                [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x30, 0x90, 0xE0, 0xFF ),
            },
            .line_size     = 20,
            .widget_gap    = 3,
            .widget_pad    = 6,
            .win_title_h   = 23,
            .win_border    = 1,
            .checkbox_sz   = 16,
            .slider_knob_w = 12,
            .min_cell_w    = 40,
            .checkmark_pad = 4,
            .cursor_w      = 1,
            .cursor_inset  = 3,
            .win_rounding    = 6,
            .widget_rounding = 4,
            .grab_rounding   = 4,
            .check_style     = GUI_CHECK_TICK,
            .bullet_style    = GUI_BULLET_DISC,
            .arrow_style     = GUI_ARROW_FILLED,
            .separator_style = GUI_SEPARATOR_SOLID,
            .progress_style  = GUI_PROGRESS_SOLID,
            .slider_knob     = GUI_SLIDER_KNOB_BAR,
            .menu_check      = GUI_MENU_CHECK_BOX,
        },
    },
};
static const u32 k_theme_count = sizeof( k_themes ) / sizeof( k_themes[ 0 ] );

/* The mutable user base style -- edited directly via gui_style_get(), or overwritten by
   gui_theme_set().  Initialized to the first built-in ("dark") so the engine is styled from
   the first frame without an explicit theme_set call. */
static gui_style_t s_style_base;

/* Active theme name -- pointer into k_themes[i].name, NULL if the user has made anonymous
   edits via gui_style_get() without subsequently calling gui_theme_set(). */
static const char* s_theme_name = NULL;

/* The active style, scaled from s_style_base for the current font size. */
static gui_style_t s_style;

gui_style_t*
gui_style_get( void )
{
    /* Direct edits via this pointer are anonymous; the caller is responsible for calling
       gui_style_apply() and is advised to call gui_theme_reset() to clear push stacks. */
    s_theme_name = NULL;
    return &s_style_base;
}

/*----------------------------------------------------------------------------------------------
    Theme API -- named style snapshots that form the root of the push/pop stack.

    gui_theme_reset() is the "large style change" escape hatch: it restores s_style_base from
    the active named theme (if any), rescales the metrics, and immediately clears both the color
    and var push stacks -- so callers never need to issue paired pop calls just to get back to a
    clean base state.

    style_new_frame() is static and lives in gui_style.c (included later in the unity build);
    declare it here so the call in gui_theme_reset() resolves within the same TU.
----------------------------------------------------------------------------------------------*/

static void style_new_frame( void );  /* forward -- defined in gui_style.c */

const gui_theme_t*
gui_theme_list( u32* count_out )
{
    if ( count_out ) *count_out = k_theme_count;
    return k_themes;
}

bool
gui_theme_set( const char* name )
{
    if ( !name ) return false;
    for ( u32 i = 0; i < k_theme_count; ++i )
    {
        if ( strcmp( name, k_themes[ i ].name ) == 0 )
        {
            s_style_base = k_themes[ i ].style;
            s_theme_name = k_themes[ i ].name;
            gui_theme_reset();
            return true;
        }
    }
    return false;
}

const char*
gui_theme_get( void )
{
    return s_theme_name;
}

void
gui_theme_reset( void )
{
    /* Restore s_style_base from the active named theme so anonymous style_get edits are
       discarded.  If no theme is set (anonymous), s_style_base is left as-is and only
       the push stacks are cleared. */
    if ( s_theme_name )
    {
        for ( u32 i = 0; i < k_theme_count; ++i )
        {
            if ( strcmp( s_theme_name, k_themes[ i ].name ) == 0 )
            {
                s_style_base = k_themes[ i ].style;
                break;
            }
        }
    }
    /* layout_compute() (called by gui_style_apply) reads the active font's metrics through the
       backend and dereferences a NULL font if none has been activated yet -- font_valid() is the
       explicit "is it safe to call in" gate for that.  When no font is set, seeding s_style_base
       is enough: whichever call activates the first font (gui_init's built-in preset, or the
       caller's own font_load) triggers gui_style_apply() and scales s_style_base at that point. */
    if ( !font_valid() )
        return;

    gui_style_apply();  /* rescale s_style from s_style_base */
    style_new_frame();  /* reseed s_col[]/s_var[] from s_style, clear all push stacks */
}

/* Recompute the active layout metrics by scaling the user's base style profile to the
   active font's type size (em).  The base style is authored assuming em=12. */
static void
layout_compute( u32 em, u32 char_h, u32 line_h )
{
    if ( em < 8u ) em = 8u;
    s_font_size = em;

    f32 scale = (f32)em / 12.0f;

    /* Copy colors and enums */
    s_style = s_style_base;

    /* Scale pixel metrics proportionally */
    s_style.line_size       = (u8)( (f32)s_style_base.line_size       * scale );
    s_style.widget_gap      = (u8)( (f32)s_style_base.widget_gap      * scale );
    s_style.widget_pad      = (u8)( (f32)s_style_base.widget_pad      * scale );
    s_style.win_title_h     = (u8)( (f32)s_style_base.win_title_h     * scale );
    s_style.win_border      = (u8)( (f32)s_style_base.win_border      * scale );
    s_style.checkbox_sz     = (u8)( (f32)s_style_base.checkbox_sz     * scale );
    s_style.slider_knob_w   = (u8)( (f32)s_style_base.slider_knob_w   * scale );
    s_style.min_cell_w      = (u8)( (f32)s_style_base.min_cell_w      * scale );
    s_style.checkmark_pad   = (u8)( (f32)s_style_base.checkmark_pad   * scale );
    s_style.cursor_w        = (u8)( (f32)s_style_base.cursor_w        * scale );
    s_style.cursor_inset    = (u8)( (f32)s_style_base.cursor_inset    * scale );
    s_style.win_rounding    = (u8)( (f32)s_style_base.win_rounding    * scale );
    s_style.widget_rounding = (u8)( (f32)s_style_base.widget_rounding * scale );
    s_style.grab_rounding   = (u8)( (f32)s_style_base.grab_rounding   * scale );

    /* Prevent vanishing outlines or cursors when scaling down. */
    if ( s_style.win_border == 0 && s_style_base.win_border > 0 ) s_style.win_border = 1u;
    if ( s_style.cursor_w == 0   && s_style_base.cursor_w > 0 )   s_style.cursor_w = 1u;
    if ( s_style.widget_gap == 0 && s_style_base.widget_gap > 0 ) s_style.widget_gap = 1u;

    /* Floor the row height to the font's glyph box and line advance so a tall-boxed font
       (e.g. one with deep descenders) never clips and a single line of text always fits. */
    if ( s_style.line_size < char_h ) s_style.line_size = (u8)( char_h );
    if ( s_style.line_size < line_h ) s_style.line_size = (u8)( line_h );
}

/* The shared stateless helpers (saturate, clampf, rect_intersect) live in gui_internal.h as
   static inline -- both units use them (gui_01_emit_draw.c needs rect_intersect for clip nesting). */

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

/* The render backend (gui_submit_shader.h, gui_load_font/gui_load_icon/gui_emit_draw/gui_emit_path/
   gui_build_tess/gui_build_cache/gui_submit_render/gui_debug_overlay .c) is the SECOND unit --
   compiled separately via gui_backend.c.  This unit calls into it through the draw_* / font_* /
   gui_render_* declarations in gui_backend.h. */

// Tier 0 -- foundation
#include "runtime_service/gui/core/gui_input.c"
#include "runtime_service/gui/core/gui_style.c"
#include "runtime_service/gui/core/gui_ctx.c"
#include "runtime_service/gui/core/gui_ctx_id.c"
#include "runtime_service/gui/core/gui_ctx_io.c"
#include "runtime_service/gui/core/gui_widget_core.c"
#include "runtime_service/gui/core/gui_stacks.c"
#include "runtime_service/gui/core/gui_symbol.c"
#include "runtime_service/gui/core/gui_resize.c"
#include "runtime_service/gui/core/gui_layout_core.c"
#include "runtime_service/gui/core/gui_layout_region.c"
#include "runtime_service/gui/core/gui_layout_child.c"
#include "runtime_service/gui/core/gui_layout.c"
#include "runtime_service/gui/core/gui_anim.c"

// Tier 1 -- leaf widgets (needs core/ only)
#include "runtime_service/gui/widgets/gui_text_edit.c"
#include "runtime_service/gui/widgets/gui_widget.c"
#include "runtime_service/gui/widgets/gui_widget_slider.c"
#include "runtime_service/gui/widgets/gui_widget_numeric.c"

// Tier 4 -- independent optional feature (needs core/ only, no window dependency)
#include "runtime_service/gui/table/gui_table.c"

// Tier 2 -- window subsystem (first real optional boundary)
#include "runtime_service/gui/window/gui_window.c"
#include "runtime_service/gui/window/gui_widget_window.c"

// Tier 3 -- window-dependent, independent of popup/
#include "runtime_service/gui/dock/gui_dock.c"

// Tier 3 -- window-dependent overlay stack (popup/nav/combo/menu share s_popups_open)
#include "runtime_service/gui/popup/gui_popup.c"
#include "runtime_service/gui/popup/gui_nav.c"
#include "runtime_service/gui/popup/gui_widget_combo.c"
#include "runtime_service/gui/popup/gui_widget_menu.c"

// Orchestration -- sits above every tier, drives whichever are compiled in
#include "runtime_service/gui/gui_frame.c"

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
// clang-format on