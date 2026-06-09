/*==============================================================================================

    runtime_service/imgui/imgui.c -- Unity build entry for the imgui module.

    Include order matters: each file can reference statics from files included above it.
        imgui_shader.h  -- embedded SPIR-V arrays (s_imgui_vert_spirv, s_imgui_frag_spirv)
        imgui_font.c    -- font atlas: font_init/shutdown/glyph + s_atlas_idx
        imgui_draw.c    -- CPU draw list: draw_reset, draw_push_*, s_draw
        imgui_render.c  -- GPU flush: render_init/shutdown/flush
        imgui_input.c   -- app->IO snapshot: input_update, s_io
        imgui_ctx.c     -- hot/active/focused state: ctx_new_frame, id_hash, rect_hit, s_ctx
        imgui_widget.c  -- widget implementations: widget_*
        imgui_api.c     -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <string.h> /* for memset */

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
    Layout
    All dimensions are integer pixel counts derived from the active style.
    Defaults match the original hardcoded constants (8x8 font, 18px line).
==============================================================================================*/

typedef struct
{
    u32 font_size;      /* rendered glyph cell side                          */
    u32 line_size;      /* widget row height                                 */
    u32 widget_gap;     /* vertical gap between consecutive widgets          */
    u32 widget_pad;     /* horizontal content area padding                   */
    u32 win_title_h;    /* window title bar height                           */
    u32 win_border;     /* window / widget outline thickness                 */
    u32 checkbox_sz;    /* checkbox indicator side                           */
    u32 slider_knob_w;  /* slider draggable knob width                       */
    u32 checkmark_pad;  /* inset of filled square inside the checkbox        */
    u32 cursor_w;       /* input text cursor width                           */
    u32 cursor_inset;   /* input text cursor top/bottom inset                */

} imgui_layout_t;

static imgui_layout_t s_layout = 
{
    .font_size     = 8,
    .line_size     = 18,
    .widget_gap    = 3,    /* 18 / 6                */
    .widget_pad    = 4,    /* 8  / 2                */
    .win_title_h   = 20,   /* 18 + 8/4              */
    .win_border    = 1,
    .checkbox_sz   = 12,   /* 8  + 8/2              */
    .slider_knob_w = 8,    /* = font_size           */
    .checkmark_pad = 3,    /* 12 / 4                */
    .cursor_w      = 1,    /* 8  / 8                */
    .cursor_inset  = 2,    /* 8  / 4                */
};

static void
layout_compute( u32 fs, u32 ls )
{
    if ( fs < 8u        ) fs = 8u;
    if ( ls < fs        ) ls = fs;

    u32 csz = fs + fs / 2u;    /* checkbox_sz = fs * 3/2 */

    s_layout.font_size     = fs;
    s_layout.line_size     = ls;
    s_layout.widget_gap    = ls / 6u < 2u ? 2u : ls / 6u;
    s_layout.widget_pad    = fs / 2u;
    s_layout.win_title_h   = ls + fs / 4u;
    s_layout.win_border    = 1u;
    s_layout.checkbox_sz   = csz;
    s_layout.slider_knob_w = fs;
    s_layout.checkmark_pad = csz / 4u;
    s_layout.cursor_w      = fs / 8u < 1u ? 1u : fs / 8u;
    s_layout.cursor_inset  = fs / 4u < 1u ? 1u : fs / 4u;
}

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/imgui/imgui_shader.h"
#include "runtime_service/imgui/imgui_font.c"
#include "runtime_service/imgui/imgui_draw.c"
#include "runtime_service/imgui/imgui_render.c"
#include "runtime_service/imgui/imgui_input.c"
#include "runtime_service/imgui/imgui_ctx.c"
#include "runtime_service/imgui/imgui_widget.c"
#include "runtime_service/imgui/imgui_api.c"

/*============================================================================================*/
