/*==============================================================================================

    runtime_service/imgui/imgui_backend.c -- Unity build entry for the imgui RENDER BACKEND.

    The second of imgui's two translation units (see imgui_backend.h for the unit split).  This
    one owns the pixel pipeline: font management, the CPU draw list, path stroking, the CPU
    tessellator, the GPU flush, and the debug overlay.  It produces no UI -- the UI unit (imgui.c)
    calls the draw_* / font_* primitives declared in imgui_backend.h, and this unit turns that
    semantic command list into vertices and submits them.

    It does NOT define the module API pointer storage (MOD_USE_RHI / MOD_USE_APP): those globals
    live in imgui.c and are fetched once at module init; this unit reads them through the same
    inline rhi() / app() accessors (extern g_*_api_ptr) from rhi_api.h / app_api.h.

    Include order matters: each file can reference statics from files included above it.

    imgui_shader.h            -- embedded SPIR-V arrays (s_imgui_vert_spirv, s_imgui_frag_spirv)
    imgui_font_builtin.c      -- hardcoded bitmap fonts: bitmap_font_def_t/t, bitmap_atlas_*, s_bitmap_*
    imgui_font.c              -- font management + dispatch: tt_font_t, tt_font_load, font_glyph, font_*
    imgui_draw.c              -- CPU draw list: draw_reset, draw_push_*, s_draw
    imgui_draw_path.c         -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    imgui_render_tess.c       -- CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    imgui_render.c            -- GPU flush: viewport_create/destroy, imgui_render_init/shutdown/flush
    imgui_debug.c             -- bolt-on debug overlay: separate draw list flushed on top (Debug only)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>      /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls imgui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/imgui/imgui_backend.h"

// clang-format off
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
#include "runtime_service/imgui/imgui_debug.c"

/*============================================================================================*/
// clang-format on
