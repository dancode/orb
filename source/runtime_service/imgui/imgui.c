/*==============================================================================================

    runtime_service/imgui/imgui.c -- Unity build entry for the imgui module.

    Include order matters: each file can reference statics from files included above it.

    imgui_shader.h            -- embedded SPIR-V arrays (s_imgui_vert_spirv, s_imgui_frag_spirv)
    imgui_font_builtin.c      -- hardcoded bitmap fonts: bitmap_font_def_t/t, bitmap_atlas_*, s_bitmap_*
    imgui_font.c              -- font management + dispatch: tt_font_t, tt_font_load, font_glyph, font_*
    imgui_draw.c              -- CPU draw list: draw_reset, draw_push_*, s_draw
    imgui_draw_path.c         -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    imgui_render_tess.c       -- CPU tessellation engine: s_tess, tess_reset, tess_dispatch, all tess_* helpers
    imgui_render.c            -- GPU flush: viewport_create/destroy, imgui_render_init/shutdown/flush
    imgui_debug.c             -- bolt-on debug overlay: separate draw list flushed on top (Debug only)
    imgui_input.c             -- app->IO snapshot: input_update, s_io
    imgui_style.c             -- style stacks: colors + metrics, style_col/style_var, push/pop/next
    imgui_ctx.c               -- context state: s_interaction, s_build, layout_frame_t, imgui_context_t, ctx_new_frame
    imgui_ctx_id.c            -- id system + state pool: id_hash, id_combine, id_seed/push/pop, imgui_state_get
    imgui_ctx_io.c            -- public IO accessors: want_capture_*, is_key_*, is_mouse_*, get_mouse_pos
    imgui_window.c            -- persistent per-window state: imgui_window_t, window_get, drag mode
    imgui_widget_core.c       -- shared widget primitives + theme: widget_behavior, COL_*, layout macros
    imgui_resize.c            -- shared edge-resize geometry: hit-test, highlight, grab, edge apply
    imgui_layout_core.c       -- layout engine: track resolver + cell emitters (widget_next_rect, grid/pack)
    imgui_layout_region.c     -- scrollable region engine: imgui_region_t, region_scrollbar, push/pop_region
    imgui_layout_child.c      -- child box + sub-layout lifecycle: begin/end_child, push/pop_layout
    imgui_layout.c            -- public layout API verbs: imgui_layout, imgui_stack, imgui_columns, imgui_grid
    imgui_text_edit.c         -- single-line text editing engine: input_field_edit (behind input_text)
    imgui_widget.c            -- core leaf widgets: text, button, checkbox, input_text, selectable
    imgui_widget_slider.c     -- slider + drag widgets: slider_float/int, drag_int, slider_render
    imgui_widget_numeric.c    -- numeric text inputs: input_int/float/double, input_float2/3/4
    imgui_widget_window.c     -- the window as a widget: begin/end_window + chrome (resize); body is a region
    imgui_popup.c             -- popups / context menus / tooltips: overlay windows on a reserved z-band
    imgui_widget_combo.c      -- combo box + list box: a popup dropdown / a scrolling child of selectables
    imgui_stack_api.c         -- push-model public API: push/pop id, item flags, style color / var
    imgui_frame.c             -- frame lifecycle: init/shutdown, new_frame, render, viewport, font, clip
    imgui_api.c               -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <stdarg.h>    /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>      /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// API internal headers
#include "runtime_service/imgui/imgui_host.h"

// API function headers
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

// API access pointers -- wired at module init/reload time
MOD_USE_RHI;
MOD_USE_APP;

// clang-format off
/*==============================================================================================
    Debug overlay build switch

    The debug overlay (imgui_debug.c) is a bolt-on second draw list, painted on top of the
    UI, that visualizes interaction rects, resize bands, window frames, and clip rects.  It
    is compiled in for Debug builds only: the build tool defines IMGUI_DEBUG_OVERLAY for the
    Debug config, but as a fallback it is also enabled from the MSVC _DEBUG macro so the
    feature tracks the configuration even before a build_tool regen.  Define
    IMGUI_NO_DEBUG_OVERLAY to force it off in any build.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( IMGUI_DEBUG_OVERLAY ) && !defined( IMGUI_NO_DEBUG_OVERLAY )
    #define IMGUI_DEBUG_OVERLAY
#endif
#if defined( IMGUI_NO_DEBUG_OVERLAY ) && defined( IMGUI_DEBUG_OVERLAY )
    #undef IMGUI_DEBUG_OVERLAY
#endif

/*----------------------------------------------------------------------------------------------
    Capture hooks

    The constituent files below call these macros at the points where the data to visualize
    is already in hand (clip pushes in imgui_draw.c, every widget rect in imgui_widget_core.c,
    window frames + resize bands in imgui_widget_window.c).  With the overlay compiled in they
    forward to the static capture functions defined in imgui_debug.c (included later in this
    unity TU -- hence the forward declarations); compiled out, they vanish to nothing, so the
    instrumentation costs zero in Release.
----------------------------------------------------------------------------------------------*/

#ifdef IMGUI_DEBUG_OVERLAY
    static void dbg_capture_widget( imgui_id_t id, imgui_rect_t r, bool hover, bool active );
    static void dbg_capture_clip  ( imgui_rect_t r, u32 depth );
    static void dbg_capture_window( imgui_rect_t r, bool is_hover );
    static void dbg_capture_resize( imgui_rect_t band, u8 hot_edges );

    #define DBG_WIDGET( id, r, hov, act ) dbg_capture_widget( ( id ), ( r ), ( hov ), ( act ) )
    #define DBG_CLIP( r, depth )          dbg_capture_clip( ( r ), ( depth ) )
    #define DBG_WINDOW( r, is_hover )     dbg_capture_window( ( r ), ( is_hover ) )
    #define DBG_RESIZE( band, hot )       dbg_capture_resize( ( band ), ( hot ) )
#else
    #define DBG_WIDGET( id, r, hov, act ) ( (void)0 )
    #define DBG_CLIP( r, depth )          ( (void)0 )
    #define DBG_WINDOW( r, is_hover )     ( (void)0 )
    #define DBG_RESIZE( band, hot )       ( (void)0 )
#endif

/*==============================================================================================
    Layout

    All dimensions are integer pixel counts derived from the active font's *type size* (em) --
    not its glyph-box height (char_h = ascent + descent), which runs ~1.3x the em and would
    inflate every padding.  The em is the design unit a typographer reasons in, so spacing,
    padding, and control heights all scale off it and stay proportional across fonts.
    Defaults match the bitmap 8x12 font (em=12).
==============================================================================================*/

typedef struct
{
    u32 line_size;      /* widget row height                                 */
    u32 widget_gap;     /* vertical gap between consecutive widgets          */
    u32 widget_pad;     /* horizontal content area padding                   */
    u32 win_title_h;    /* window title bar height                           */
    u32 win_border;     /* window / widget outline thickness                 */
    u32 checkbox_sz;    /* checkbox indicator side                           */
    u32 slider_knob_w;  /* slider draggable knob width                       */
    u32 min_cell_w;     /* floor a flex/fraction track shrinks to before overflow */
    u32 checkmark_pad;  /* inset of filled square inside the checkbox        */
    u32 cursor_w;       /* input text cursor width                           */
    u32 cursor_inset;   /* input text cursor top/bottom inset                */

} imgui_metrics_t;

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
    .cursor_w      = 1,    /* 12 / 10                    */
    .cursor_inset  = 3,    /* 12 / 4                     */
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
    u32 row = em + 2u * pad_y;
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
}

/*==============================================================================================
    Shared scalar + geometry helpers

    Small stateless helpers used across the constituent files below.  They live here, ahead of
    the unity includes, so every file (starting with imgui_draw.c) can use them.  rect_hit needs
    the input snapshot and so stays in imgui_ctx.c; these need nothing but their arguments.
==============================================================================================*/

/* Clamp t to [0,1] -- the saturate used by slider + scrollbar drag mapping. */
static f32 
saturate( f32 t ) { return t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t ); }

/* Clamp v to [lo,hi]. */
static f32 
clampf( f32 v, f32 lo, f32 hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

/* Overlap of two rects (zero-size when they do not overlap).  Nested regions intersect their
   clip with the parent so a child never scissors or hit-tests past it. */
static imgui_rect_t
rect_intersect( imgui_rect_t a, imgui_rect_t b )
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = ( a.x + a.w < b.x + b.w ) ? a.x + a.w : b.x + b.w;
    f32 y1 = ( a.y + a.h < b.y + b.h ) ? a.y + a.h : b.y + b.h;
    f32 w  = x1 - x0 > 0.0f ? x1 - x0 : 0.0f;
    f32 h  = y1 - y0 > 0.0f ? y1 - y0 : 0.0f;
    return ( imgui_rect_t ){ x0, y0, w, h };
}

/*==============================================================================================
    Internal record types shared into imgui_context_t

    A few per-context record types are defined here, ahead of the unity includes, because the
    context struct (imgui_ctx.c) embeds them by value and is itself referenced from files included
    both before and after it.  Their behavior (the table, drag, raise-to-front) stays in the owning
    .c file; only the layout lives here so the context can hold it.
==============================================================================================*/

#define IMGUI_MAX_WINDOWS 32

/* One persisted window.  Geometry is owned here after the first appearance; the window pool that
   holds these lives in the bound context (imgui_context_t).  Behavior is in imgui_window.c. */
typedef struct imgui_window_t
{
    imgui_id_t id;              /* id_hash(title); 0 = free slot                  */
    f32        x, y;            /* persisted top-left (updated by dragging)       */
    f32        w, h;            /* persisted dimensions                           */
    u32        z;               /* paint order: higher = more recently raised = in front */
    u32        viewport;        /* target surface index (0 = main swapchain); set via set_next_window_viewport */

    f32        scroll_y;        /* vertical scroll offset; 0 = top                */
    f32        scroll_x;        /* horizontal scroll offset; 0 = left             */
    f32        content_h;       /* total content height measured last frame       */
    f32        content_w;       /* total content width measured last frame        */

    bool       collapsed;       /* title-bar-only when set; toggled by the arrow  */

    imgui_win_flags_t flags;    /* behavior flags supplied to begin_window        */

    /* Next-window channel bookkeeping (see set_next_window_pos / _size).  last_frame drives the
       "appearing" test; the allow masks track which conditions a queued value may still fire. */

    u32        last_frame;      /* frame index last begun; 0 = never begun        */
    u8         set_pos_allow;   /* conds still permitted to set position (imgui_cond_t bits) */
    u8         set_size_allow;  /* conds still permitted to set size              */

} imgui_window_t;

/*==============================================================================================
    Forward declarations across the unity boundary

    The mouse-input path (imgui_input.c) resolves an event's app win_id to the viewport hosting it,
    but the viewport pool lives on the bound context (g_ctx, imgui_ctx.c) which is included later.
    One TU, so a forward declaration here lets input.c call the resolver defined after g_ctx.
==============================================================================================*/

static u32 viewport_index_for_window( i32 win_id );

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/imgui/imgui_shader.h"
#include "runtime_service/imgui/imgui_font_builtin.c"
#include "runtime_service/imgui/imgui_font.c"
#include "runtime_service/imgui/imgui_draw.c"
#include "runtime_service/imgui/imgui_draw_path.c"
#include "runtime_service/imgui/imgui_render_tess.c"
#include "runtime_service/imgui/imgui_render.c"
#include "runtime_service/imgui/imgui_input.c"
#include "runtime_service/imgui/imgui_style.c"
#include "runtime_service/imgui/imgui_ctx.c"
#include "runtime_service/imgui/imgui_ctx_id.c"
#include "runtime_service/imgui/imgui_ctx_io.c"
#include "runtime_service/imgui/imgui_debug.c"
#include "runtime_service/imgui/imgui_window.c"
#include "runtime_service/imgui/imgui_widget_core.c"
#include "runtime_service/imgui/imgui_resize.c"
#include "runtime_service/imgui/imgui_layout_core.c"
#include "runtime_service/imgui/imgui_layout_region.c"
#include "runtime_service/imgui/imgui_layout_child.c"
#include "runtime_service/imgui/imgui_layout.c"
#include "runtime_service/imgui/imgui_text_edit.c"
#include "runtime_service/imgui/imgui_widget.c"
#include "runtime_service/imgui/imgui_widget_slider.c"
#include "runtime_service/imgui/imgui_widget_numeric.c"
#include "runtime_service/imgui/imgui_widget_window.c"
#include "runtime_service/imgui/imgui_popup.c"
#include "runtime_service/imgui/imgui_nav.c"
#include "runtime_service/imgui/imgui_widget_combo.c"
#include "runtime_service/imgui/imgui_widget_menu.c"
#include "runtime_service/imgui/imgui_stack_api.c"
#include "runtime_service/imgui/imgui_frame.c"

#ifndef IMGUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/imgui/imgui_api.c"
#endif

/*============================================================================================*/
// clang-format on