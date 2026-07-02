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

    gui_submit_shader.h     -- embedded SPIR-V arrays (s_gui_vert_spirv, s_gui_frag_spirv)
    gui_load_atlas.h/.c     -- shared GPU-atlas asset: gui_atlas_t, gui_atlas_create/upload/destroy
    gui_load_font.h         -- shared font types: font_metrics_t, font_slot_t
    gui_load_font_orb.c     -- .orb_font loader (font_slot_load, font_slot_glyph)
    gui_load_font.c         -- registry + dispatch: font_slot_t, font_load/use, font_glyph
    gui_load_icon.c         -- runtime icon atlas: icon_register/find/get, draw_push_icon
    gui_emit_draw.c         -- CPU draw list: draw_reset, draw_push_*, s_draw
    gui_emit_path.c         -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    gui_build_tess.c        -- CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    gui_build_cache.c       -- retained frame-geometry cache (BUILD): cache_build_frame, s_cache, s_dispatch
    gui_submit_render.c     -- GPU resources + flush (SUBMIT): viewport_create/destroy, init/shutdown/flush
    gui_debug_overlay.c     -- bolt-on debug overlay: separate draw list flushed on top (Debug only)

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

// Tier 0 -- Foundation: types and embedded shader bytecode only, no logic.
#include "runtime_service/gui/backend/gui_submit_shader.h"

// Tier 1 -- Resource registries: fonts + icons, on a shared GPU-atlas helper (gui_load_atlas).
// Independent of each other; each owns its own atlas instance, CPU staging, and deferred-upload
// lifecycle -- gui_load_atlas.h/.c only factors out the create/upload/destroy sequence they share.
#include "runtime_service/gui/backend/gui_load_atlas.h"
#include "runtime_service/gui/backend/gui_load_atlas.c"
#include "runtime_service/gui/backend/gui_load_font.h"
#include "runtime_service/gui/backend/gui_load_font_orb.c"
#include "runtime_service/gui/backend/gui_load_font.c"
#include "runtime_service/gui/backend/gui_load_icon.c"

// Tier 2 -- EMIT: the semantic draw list (s_draw) and the line/path stroker built on it.
#include "runtime_service/gui/backend/gui_emit_draw.c"
#include "runtime_service/gui/backend/gui_emit_path.c"

// Tier 3 -- BUILD, part A: tessellation primitives (gui_cmd_t -> s_tess geometry).  
// No public surface -- driven entirely from Tier 4 (cache_tess_window / cache_build_frame).
#include "runtime_service/gui/backend/gui_build_tess.c"

// Tier 4 -- BUILD, part B: retained cache & orchestration (diff, reuse-or-tessellate, z-sort).
#include "runtime_service/gui/backend/gui_build_cache.c"

// Tier 5 -- SUBMIT: GPU resource lifecycle + the per-surface flush.
#include "runtime_service/gui/backend/gui_submit_render.c"

// Tier 6 -- Debug overlay: a parallel mini-pipeline, compiled out unless GUI_DEBUG_OVERLAY.
#include "runtime_service/gui/backend/gui_debug_overlay.c"

/*============================================================================================*/
// clang-format on
