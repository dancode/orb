/*==============================================================================================

    runtime_service/gui/gui.c -- Unity build entry for the gui UI / core unit.

    gui is two translation units linked into one static lib (see gui_backend.h):
      - this unit (gui.c): context, layout, widgets, chrome, popups, nav, input, frame
        lifecycle, the module vtable.  Owns s_build / s_io / s_interaction / g_ctx and the stacks.
      - the render backend (gui_backend.c): fonts, draw list, tessellation, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_font / s_render.  Called through gui_backend.h.

    Include order matters: each file can reference statics from files included above it.

    gui_input.c             -- app->IO snapshot: input_update, s_io
    gui_style.c             -- style stacks: colors + metrics, style_col/style_var, push/pop/next
    gui_ctx.c               -- context state: s_interaction, s_build, layout_frame_t, gui_context_t, ctx_new_frame
    gui_ctx_id.c            -- id system + state pool: id_hash, id_combine, id_seed/push/pop, gui_state_get
    gui_ctx_io.c            -- public IO accessors: want_capture_*, is_key_*, is_mouse_*, get_mouse_pos
    gui_window.c            -- persistent per-window state: gui_window_t, window_get, drag mode
    gui_widget_core.c       -- shared widget primitives + theme: widget_behavior, COL_*, layout macros
    gui_symbol.c            -- symbol + shape draw primitives: draw_arrow/check/frame/round_rect/arc/...
    gui_resize.c            -- shared edge-resize geometry: hit-test, highlight, grab, edge apply
    gui_layout_core.c       -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    gui_layout_region.c     -- scrollable region engine: gui_region_t, region_scrollbar, push/pop_region
    gui_layout_child.c      -- child box + sub-layout lifecycle: begin/child_end, push/pop_layout
    gui_layout.c            -- public layout API verbs: gui_layout, gui_stack, gui_cols, gui_grid
    gui_text_edit.c         -- single-line text editing engine: input_field_edit (behind input_text)
    gui_anim.c              -- widget animation utilities: gui_anim_f32, gui_anim_bg
    gui_widget.c            -- core leaf widgets: text, button, checkbox, input_text, selectable
    gui_widget_slider.c     -- slider + drag widgets: slider_float/int, drag_int, slider_render
    gui_widget_numeric.c    -- numeric text inputs: input_int/float/double, input_float2/3/4
    gui_widget_window.c     -- the window as a widget: begin/window_end + chrome (resize); body is a region
    gui_dock.c              -- docking: dock-node tree, splitters, tab strips, dockspace + build API
    gui_popup.c             -- popups / context menus / tooltips: overlay windows on a reserved z-band
    gui_widget_combo.c      -- combo box + list box: a popup dropdown / a scrolling child of selectables
    gui_stacks.c            -- push-model public API: push/pop id, item flags, style color / var
    gui_table.c             -- table layout: multi-column rows with cell clipping
    gui_frame.c             -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, viewport, font, clip
    gui_api.c               -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>    /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>      /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls gui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/gui/gui_backend.h"

// API function headers
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

// API access pointers -- wired at module init/reload time
MOD_USE_RHI;
MOD_USE_APP;

// clang-format off

/* The debug-overlay build switch (GUI_DEBUG_OVERLAY) and the DBG_* capture macros live in
   gui_backend.h: both units (this one and gui_backend.c, which defines the capture targets)
   must agree on them.  The widget / chrome files below invoke DBG_WIDGET / DBG_WINDOW / DBG_RESIZE;
   in Debug they call across to the overlay's capture functions in the backend unit. */

/*==============================================================================================
    Layout

    All dimensions are integer pixel counts derived from the active font's *type size* (em) --
    not its glyph-box height (char_h = ascent + descent), which runs ~1.3x the em and would
    inflate every padding.  The em is the design unit a typographer reasons in, so spacing,
    padding, and control heights all scale off it and stay proportional across fonts.
    Defaults match the bitmap 8x12 font (em=12).
==============================================================================================*/

/* gui_metrics_t (the layout metric record) is defined in gui_internal.h. */

/* Font type size (em) used by layout_compute; updated by font_set_builtin() / font_load(). */
static u32 s_font_size = 0;

/* The base style profile authored for em=12. Users edit this via gui_style_get(). */
static gui_style_t s_style_base =
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
    .line_size     = 20,   /* 12 + 2*(12/3)              */
    .widget_gap    = 3,    /* 12 / 4                     */
    .widget_pad    = 6,    /* 12 / 2                     */
    .win_title_h   = 23,   /* 20 + 12/4                  */
    .win_border    = 1,
    .checkbox_sz   = 16,   /* 20 * 4/5                   */
    .slider_knob_w = 12,   /* = em                       */
    .min_cell_w    = 40,   /* 20 * 2                     */
    .checkmark_pad = 4,    /* 16 / 4                     */
    .cursor_w        = 1,  /* 12 / 10                    */
    .cursor_inset    = 3,  /* 12 / 4                     */
    .win_rounding    = 6,  /* 12 / 2 -- windows a touch rounder than controls */
    .widget_rounding = 4,  /* 12 / 3 -- gentle control frame radius          */
    .grab_rounding   = 4,  /* 12 / 3 -- knobs / grabs (raise for pill grabs)  */
    .check_style     = GUI_CHECK_TICK,    /* 'v' tick by default (set_check_style to change)  */
    .bullet_style    = GUI_BULLET_DISC,   /* filled disc by default (set_bullet_style to change) */
    .arrow_style     = GUI_ARROW_FILLED,  /* solid triangle by default (set_arrow_style to change) */
    .separator_style = GUI_SEPARATOR_SOLID,  /* solid rule by default (push GUI_VAR_SEPARATOR_STYLE) */
    .progress_style  = GUI_PROGRESS_SOLID,   /* flat fill by default (push GUI_VAR_PROGRESS_STYLE)   */
    .slider_knob     = GUI_SLIDER_KNOB_BAR,  /* bar grab by default (push GUI_VAR_SLIDER_KNOB)       */
    .menu_check      = GUI_MENU_CHECK_BOX,   /* bordered box by default (push GUI_VAR_MENU_CHECK)    */
};

/* The active style, scaled from s_style_base for the current font size. */
static gui_style_t s_style;

gui_style_t* gui_style_get( void )
{
    return &s_style_base;
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
   static inline -- both units use them (gui_draw.c needs rect_intersect for clip nesting). */

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

/* The render backend (gui_shader.h, gui_font_builtin/font/draw/draw_path/render_tess/render/
   debug .c) is the SECOND unit -- compiled separately via gui_backend.c.  This unit calls into
   it through the draw_* / font_* / gui_render_* declarations in gui_backend.h. */

#include "runtime_service/gui/gui_input.c"
#include "runtime_service/gui/gui_style.c"
#include "runtime_service/gui/gui_ctx.c"
#include "runtime_service/gui/gui_ctx_id.c"
#include "runtime_service/gui/gui_ctx_io.c"
#include "runtime_service/gui/gui_window.c"
#include "runtime_service/gui/gui_widget_core.c"
#include "runtime_service/gui/gui_symbol.c"
#include "runtime_service/gui/gui_resize.c"
#include "runtime_service/gui/gui_layout_core.c"
#include "runtime_service/gui/gui_layout_region.c"
#include "runtime_service/gui/gui_layout_child.c"
#include "runtime_service/gui/gui_layout.c"
#include "runtime_service/gui/gui_text_edit.c"
#include "runtime_service/gui/gui_anim.c"
#include "runtime_service/gui/gui_widget.c"
#include "runtime_service/gui/gui_widget_slider.c"
#include "runtime_service/gui/gui_widget_numeric.c"
#include "runtime_service/gui/gui_widget_window.c"
#include "runtime_service/gui/gui_dock.c"
#include "runtime_service/gui/gui_popup.c"
#include "runtime_service/gui/gui_nav.c"
#include "runtime_service/gui/gui_widget_combo.c"
#include "runtime_service/gui/gui_widget_menu.c"
#include "runtime_service/gui/gui_stacks.c"
#include "runtime_service/gui/gui_table.c"
#include "runtime_service/gui/gui_frame.c"

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
// clang-format on