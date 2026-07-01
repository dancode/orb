/*==============================================================================================

    runtime_service/gui/gui_backend.c -- Unity build entry for the gui RENDER BACKEND.

    The second of gui's two translation units (see gui_backend.h for the unit split).  This
    one owns the pixel pipeline: font management, the CPU draw list, path stroking, the CPU
    tessellator, the GPU flush, and the debug overlay.  It produces no UI -- the UI unit (gui.c)
    calls the draw_* / font_* primitives declared in gui_backend.h, and this unit turns that
    semantic command list into vertices and submits them.

    It does NOT define the module API pointer storage (MOD_USE_RHI / MOD_USE_APP): those globals
    live in gui.c and are fetched once at module init; this unit reads them through the same
    inline rhi() / app() accessors (extern g_*_api_ptr) from rhi_api.h / app_api.h.

    Include order matters: each file can reference statics from files included above it.

    gui_shader.h            -- embedded SPIR-V arrays (s_gui_vert_spirv, s_gui_frag_spirv)
    gui_font.h              -- shared font types: font_metrics_t, font_slot_t
    gui_font_ttf.c          -- proportional .orb_font loader (ttf_load_file, ttf_glyph)
    gui_font.c              -- registry + dispatch: font_slot_t, font_load/use, font_glyph
    gui_draw.c              -- CPU draw list: draw_reset, draw_push_*, s_draw
    gui_draw_path.c         -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    gui_render_tess.c       -- CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    gui_render_cache.c      -- retained frame-geometry cache (BUILD): cache_build_frame, s_cache, s_dispatch
    gui_render.c            -- GPU resources + flush (SUBMIT): viewport_create/destroy, init/shutdown/flush
    gui_debug.c             -- bolt-on debug overlay: separate draw list flushed on top (Debug only)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls gui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/gui/gui_backend.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/gui/backend/gui_shader.h"
#include "runtime_service/gui/backend/gui_font.h"
#include "runtime_service/gui/backend/gui_font_ttf.c"
#include "runtime_service/gui/backend/gui_font.c"
#include "runtime_service/gui/backend/gui_icon.c"
#include "runtime_service/gui/backend/gui_draw.c"
#include "runtime_service/gui/backend/gui_draw_path.c"
#include "runtime_service/gui/backend/gui_render_tess.c"
#include "runtime_service/gui/backend/gui_render_cache.c"
#include "runtime_service/gui/backend/gui_render.c"
#include "runtime_service/gui/backend/gui_debug.c"

/*============================================================================================*/
// clang-format on
