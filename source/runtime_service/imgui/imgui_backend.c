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
    imgui_font.h              -- shared font types: font_metrics_t, bit_font_def_t, font_slot_t
    imgui_font_bmp.c          -- bmp fonts: bit-font -> R8 grid atlas (bmp_font_t, bmp_select, bmp_glyph)
    imgui_font_ttf.c          -- ttf fonts: proportional .orb_font loader (ttf_load_file, ttf_glyph)
    imgui_font.c              -- neutral registry + dispatch: font_slot_t, font_load/use, font_glyph
    imgui_draw.c              -- CPU draw list: draw_reset, draw_push_*, s_draw
    imgui_draw_path.c         -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    imgui_render_tess.c       -- CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    imgui_render.c            -- GPU flush: viewport_create/destroy, imgui_render_init/shutdown/flush
    imgui_debug.c             -- bolt-on debug overlay: separate draw list flushed on top (Debug only)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls imgui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/imgui/imgui_backend.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/imgui/backend/imgui_shader.h"
#include "runtime_service/imgui/backend/imgui_font.h"
#include "runtime_service/imgui/backend/imgui_font_bmp.c"
#include "runtime_service/imgui/backend/imgui_font_ttf.c"
#include "runtime_service/imgui/backend/imgui_font.c"
#include "runtime_service/imgui/backend/imgui_icon.c"
#include "runtime_service/imgui/backend/imgui_draw.c"
#include "runtime_service/imgui/backend/imgui_draw_path.c"
#include "runtime_service/imgui/backend/imgui_render_tess.c"
#include "runtime_service/imgui/backend/imgui_render.c"
#include "runtime_service/imgui/backend/imgui_debug.c"

/*============================================================================================*/
// clang-format on
