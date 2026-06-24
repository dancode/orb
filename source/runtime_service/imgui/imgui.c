/*==============================================================================================

    runtime_service/imgui/imgui.c -- Unity build entry for the imgui UI / core unit.

    imgui is two translation units linked into one static lib (see imgui_backend.h):
      - this unit (imgui.c): context, layout, widgets, chrome, popups, nav, input, frame
        lifecycle, the module vtable.  Owns s_build / s_io / s_interaction / g_ctx and the stacks.
      - the render backend (imgui_backend.c): fonts, draw list, tessellation, GPU flush, debug
        overlay.  Owns s_draw / s_tess / s_font / s_render.  Called through imgui_backend.h.

    Include order matters: each file can reference statics from files included above it.

    imgui_input.c             -- app->IO snapshot: input_update, s_io
    imgui_style.c             -- style stacks: colors + metrics, style_col/style_var, push/pop/next
    imgui_ctx.c               -- context state: s_interaction, s_build, layout_frame_t, imgui_context_t, ctx_new_frame
    imgui_ctx_id.c            -- id system + state pool: id_hash, id_combine, id_seed/push/pop, imgui_state_get
    imgui_ctx_io.c            -- public IO accessors: want_capture_*, is_key_*, is_mouse_*, get_mouse_pos
    imgui_window.c            -- persistent per-window state: imgui_window_t, window_get, drag mode
    imgui_widget_core.c       -- shared widget primitives + theme: widget_behavior, COL_*, layout macros
    imgui_draw_symbol.c       -- symbol + shape render primitives: draw_arrow/check/frame/round_rect/arc/..., render_*
    imgui_resize.c            -- shared edge-resize geometry: hit-test, highlight, grab, edge apply
    imgui_layout_core.c       -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    imgui_layout_region.c     -- scrollable region engine: imgui_region_t, region_scrollbar, push/pop_region
    imgui_layout_child.c      -- child box + sub-layout lifecycle: begin/end_child, push/pop_layout
    imgui_layout.c            -- public layout API verbs: imgui_layout, imgui_stack, imgui_columns, imgui_grid
    imgui_text_edit.c         -- single-line text editing engine: input_field_edit (behind input_text)
    imgui_anim.c              -- widget animation utilities: imgui_anim_f32, imgui_anim_bg
    imgui_widget.c            -- core leaf widgets: text, button, checkbox, input_text, selectable
    imgui_widget_slider.c     -- slider + drag widgets: slider_float/int, drag_int, slider_render
    imgui_widget_numeric.c    -- numeric text inputs: input_int/float/double, input_float2/3/4
    imgui_widget_window.c     -- the window as a widget: begin/end_window + chrome (resize); body is a region
    imgui_dock.c              -- docking: dock-node tree, splitters, tab strips, dockspace + build API
    imgui_popup.c             -- popups / context menus / tooltips: overlay windows on a reserved z-band
    imgui_widget_combo.c      -- combo box + list box: a popup dropdown / a scrolling child of selectables
    imgui_stack_api.c         -- push-model public API: push/pop id, item flags, style color / var
    imgui_table.c             -- table layout: multi-column rows with cell clipping
    imgui_frame.c             -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, viewport, font, clip
    imgui_api.c               -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>    /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>      /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls imgui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/imgui/imgui_backend.h"

// API function headers
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

// API access pointers -- wired at module init/reload time
MOD_USE_RHI;
MOD_USE_APP;

// clang-format off

/* The debug-overlay build switch (IMGUI_DEBUG_OVERLAY) and the DBG_* capture macros live in
   imgui_backend.h: both units (this one and imgui_backend.c, which defines the capture targets)
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

/* imgui_metrics_t (the layout metric record) is defined in imgui_internal.h. */

/* Font type size (em) used by layout_compute; updated by set_font() / load_font(). */
static u32 s_font_size = 0;

/* Default values -- the em=12 bitmap result of layout_compute, for the pre-font-load state. */
static imgui_metrics_t s_layout =
{
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
    .check_style     = IMGUI_CHECK_TICK,    /* 'v' tick by default (set_check_style to change)  */
    .bullet_style    = IMGUI_BULLET_DISC,   /* filled disc by default (set_bullet_style to change) */
    .arrow_style     = IMGUI_ARROW_FILLED,  /* solid triangle by default (set_arrow_style to change) */
    .separator_style = IMGUI_SEPARATOR_SOLID,  /* solid rule by default (push IMGUI_VAR_SEPARATOR_STYLE) */
    .progress_style  = IMGUI_PROGRESS_SOLID,   /* flat fill by default (push IMGUI_VAR_PROGRESS_STYLE)   */
    .slider_knob     = IMGUI_SLIDER_KNOB_BAR,  /* bar grab by default (push IMGUI_VAR_SLIDER_KNOB)       */
};

/* Recompute the layout metrics from a font's type size (em), glyph-box height (char_h), and
   line advance (line_h).  Called by set_font() / load_font() / set_bmp_scale().  All paddings
   scale off the em; the row height is floored to char_h and line_h so glyphs never clip. */

static void
layout_compute( u32 em, u32 char_h, u32 line_h )
{
    if ( em < 8u ) em = 8u;
    s_font_size = em;

    /* Frame padding around a control's text, scaled off the em.  Vertical padding is the
       dominant aesthetic lever: a control's row is the em plus this top and bottom, so text
       sits optically centered with breathing room instead of crowding the frame edge. */
    u32 pad_y = em / 3u;                       /* ~0.33 em above / below the glyph */

    /* Row height = em + symmetric vertical padding, then floored to the font's glyph box and
       line advance so a tall-boxed font (e.g. one with deep descenders) never clips and a
       single line of text always fits.  This is THE knob the other heights cascade from. */

    u32  row = em + 2u * pad_y;
    if ( row < char_h ) row = char_h;
    if ( row < line_h ) row = line_h;

    /* Checkbox indicator: 4/5 of the row leaves a small margin top and bottom and tracks it. */
    u32 csz = ( row * 4u ) / 5u;

    s_layout.line_size     = row;
    s_layout.widget_gap    = em / 4u < 2u ? 2u : em / 4u;   /* ~0.25 em between stacked rows */
    s_layout.widget_pad    = em / 2u;                       /* horizontal text inset (~0.5 em) */
    s_layout.win_title_h   = row + em / 4u;                 /* a touch taller than a body row */
    s_layout.win_border    = 1u;
    s_layout.checkbox_sz   = csz;
    s_layout.slider_knob_w = em;
    /* Smallest width a shrinking flex/fraction track stops at before it overflows the row
       (clipped at the region edge).  Two row-heights keeps a control just grabbable; fixed-px
       tracks are never floored -- an explicit pixel size is the author's choice. */
    s_layout.min_cell_w    = row * 2u;
    s_layout.checkmark_pad = csz / 4u;
    s_layout.cursor_w      = em / 10u < 1u ? 1u : em / 10u; /* thin caret (~0.1 em) */
    s_layout.cursor_inset  = em / 4u  < 1u ? 1u : em / 4u;  /* caret top/bottom inset */
    s_layout.win_rounding    = em / 2u;                     /* windows (~0.5 em)   */
    s_layout.widget_rounding = em / 3u;                     /* control frames (~0.33 em) */
    s_layout.grab_rounding   = em / 3u;                     /* knobs / grabs (~0.33 em) */
}

/* The shared stateless helpers (saturate, clampf, rect_intersect) live in imgui_internal.h as
   static inline -- both units use them (imgui_draw.c needs rect_intersect for clip nesting). */

/*==============================================================================================
    Internal record types shared into imgui_context_t

    The per-context record types (imgui_window_t, layout_frame_t, imgui_popup_t, imgui_viewport_t,
    imgui_context_t, ...) and the cross-unity-boundary forward declarations now live in
    imgui_internal.h, included at the top of this file -- so every constituent file below sees the
    full shared type layer up front rather than relying on include order.  Their behavior stays in
    the owning .c file.
==============================================================================================*/

/*==============================================================================================
    Unity build
==============================================================================================*/

/* The render backend (imgui_shader.h, imgui_font_builtin/font/draw/draw_path/render_tess/render/
   debug .c) is the SECOND unit -- compiled separately via imgui_backend.c.  This unit calls into
   it through the draw_* / font_* / imgui_render_* declarations in imgui_backend.h. */

#include "runtime_service/imgui/imgui_input.c"
#include "runtime_service/imgui/imgui_style.c"
#include "runtime_service/imgui/imgui_ctx.c"
#include "runtime_service/imgui/imgui_ctx_id.c"
#include "runtime_service/imgui/imgui_ctx_io.c"
#include "runtime_service/imgui/imgui_window.c"
#include "runtime_service/imgui/imgui_widget_core.c"
#include "runtime_service/imgui/imgui_draw_symbol.c"
#include "runtime_service/imgui/imgui_resize.c"
#include "runtime_service/imgui/imgui_layout_core.c"
#include "runtime_service/imgui/imgui_layout_region.c"
#include "runtime_service/imgui/imgui_layout_child.c"
#include "runtime_service/imgui/imgui_layout.c"
#include "runtime_service/imgui/imgui_text_edit.c"
#include "runtime_service/imgui/imgui_anim.c"
#include "runtime_service/imgui/imgui_widget.c"
#include "runtime_service/imgui/imgui_widget_slider.c"
#include "runtime_service/imgui/imgui_widget_numeric.c"
#include "runtime_service/imgui/imgui_widget_window.c"
#include "runtime_service/imgui/imgui_dock.c"
#include "runtime_service/imgui/imgui_popup.c"
#include "runtime_service/imgui/imgui_nav.c"
#include "runtime_service/imgui/imgui_widget_combo.c"
#include "runtime_service/imgui/imgui_widget_menu.c"
#include "runtime_service/imgui/imgui_stack_api.c"
#include "runtime_service/imgui/imgui_table.c"
#include "runtime_service/imgui/imgui_frame.c"

#ifndef IMGUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/imgui/imgui_api.c"
#endif

/*============================================================================================*/
// clang-format on