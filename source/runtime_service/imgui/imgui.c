/*==============================================================================================

    runtime_service/imgui/imgui.c -- Unity build entry for the imgui module.

    Include order matters: each file can reference statics from files included above it.
        imgui_shader.h       -- embedded SPIR-V arrays (s_imgui_vert_spirv, s_imgui_frag_spirv)
        imgui_font_builtin.c -- hardcoded bitmap fonts: bitmap_font_def_t/t, bitmap_atlas_*, s_bitmap_*
        imgui_font.c         -- font management + dispatch: tt_font_t, tt_font_load, font_glyph, font_*
        imgui_draw.c         -- CPU draw list: draw_reset, draw_push_*, s_draw
        imgui_draw_path.c    -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
        imgui_render.c  -- GPU flush: imgui_render_init/shutdown/flush
        imgui_debug.c   -- bolt-on debug overlay: separate draw list flushed on top (Debug only)
        imgui_input.c   -- app->IO snapshot: input_update, s_io
        imgui_style.c   -- style stacks: colors + metrics, style_col/style_var, push/pop/next
        imgui_ctx.c     -- hot/active/focused state: ctx_new_frame, id_hash, rect_hit, s_ctx
        imgui_window.c       -- persistent per-window state: imgui_window_t, window_get, drag mode
        imgui_widget_core.c  -- shared widget primitives + theme: widget_behavior, COL_*, layout macros
        imgui_layout.c       -- layout-region engine: region pool, scrollbar, push/pop_region, begin/end_child
        imgui_widget.c       -- leaf widgets: text, button, checkbox, slider, input_text, selectable
        imgui_widget_window.c-- the window as a widget: begin/end_window + chrome (resize); body is a region
        imgui_popup.c        -- popups / context menus / tooltips: overlay windows on a reserved z-band
        imgui_api.c     -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>    /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>      /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// internal API headers
#include "engine/mod/mod_export.h"
#include "runtime_service/imgui/imgui_host.h"

// API function headers
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

// API access pointers -- wired at module init/reload time
MOD_USE_RHI;
MOD_USE_APP;

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

    All dimensions are integer pixel counts derived from the active font and line_size.
    Defaults match the bitmap 8x12 font (fs=12) with a 20px line height.
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

/* Font size used by layout_compute; updated by set_font() / load_font(). */
static u32 s_font_size = 0;

/* Default values */
static imgui_metrics_t s_layout =
{
    .line_size     = 20,
    .widget_gap    = 3,    /* 20 / 6                     */
    .widget_pad    = 6,    /* 12 / 2  (fs=12 default)    */
    .win_title_h   = 23,   /* 20 + 12/4                  */
    .win_border    = 1,
    .checkbox_sz   = 18,   /* 12 + 12/2                  */
    .slider_knob_w = 12,   /* = fs                       */
    .min_cell_w    = 40,   /* 20 * 2                     */
    .checkmark_pad = 4,    /* 18 / 4                     */
    .cursor_w      = 1,    /* 12 / 8                     */
    .cursor_inset  = 3,    /* 12 / 4                     */
};

/* Calculate new layout values based on the given line size and current font size.  
   Called by set_font() and load_font(). */

static void
layout_compute( u32 ls )
{
    u32 fs = s_font_size;
    if ( fs < 8u ) fs = 8u;
    if ( ls < fs ) ls = fs;

    /* Checkbox indicator must fit inside the widget row, so size it from the row
       height (line_size), not the glyph height -- 4/5 of the row leaves a small
       margin top and bottom and scales with the font. */
    u32 csz = ( ls * 4u ) / 5u;

    s_layout.line_size     = ls;
    s_layout.widget_gap    = ls / 6u < 2u ? 2u : ls / 6u;
    s_layout.widget_pad    = fs / 2u;
    s_layout.win_title_h   = ls + fs / 4u;
    s_layout.win_border    = 1u;
    s_layout.checkbox_sz   = csz;
    s_layout.slider_knob_w = fs;
    /* Smallest width a shrinking flex/fraction track stops at before it overflows the row
       (clipped at the region edge).  Two row-heights keeps a control just grabbable; fixed-px
       tracks are never floored -- an explicit pixel size is the author's choice. */
    s_layout.min_cell_w    = ls * 2u;
    s_layout.checkmark_pad = csz / 4u;
    s_layout.cursor_w      = fs / 8u < 1u ? 1u : fs / 8u;
    s_layout.cursor_inset  = fs / 4u < 1u ? 1u : fs / 4u;
}

/*==============================================================================================
    Shared scalar + geometry helpers

    Small stateless helpers used across the constituent files below.  They live here, ahead of
    the unity includes, so every file (starting with imgui_draw.c) can use them.  rect_hit needs
    the input snapshot and so stays in imgui_ctx.c; these need nothing but their arguments.
==============================================================================================*/

/* Clamp t to [0,1] -- the saturate used by slider + scrollbar drag mapping. */
static f32 saturate( f32 t ) { return t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t ); }

/* Clamp v to [lo,hi]. */
static f32 clampf( f32 v, f32 lo, f32 hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

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
    Unity build
==============================================================================================*/

#include "runtime_service/imgui/imgui_shader.h"
#include "runtime_service/imgui/imgui_font_builtin.c"
#include "runtime_service/imgui/imgui_font.c"
#include "runtime_service/imgui/imgui_draw.c"
#include "runtime_service/imgui/imgui_draw_path.c"
#include "runtime_service/imgui/imgui_render.c"
#include "runtime_service/imgui/imgui_debug.c"
#include "runtime_service/imgui/imgui_input.c"
#include "runtime_service/imgui/imgui_style.c"
#include "runtime_service/imgui/imgui_ctx.c"
#include "runtime_service/imgui/imgui_window.c"
#include "runtime_service/imgui/imgui_widget_core.c"
#include "runtime_service/imgui/imgui_layout.c"
#include "runtime_service/imgui/imgui_widget.c"
#include "runtime_service/imgui/imgui_widget_window.c"
#include "runtime_service/imgui/imgui_popup.c"

#ifndef IMGUI_API_C_PRELUDE
    #include "runtime_service/imgui/imgui_api.c"
#endif

/*============================================================================================*/
