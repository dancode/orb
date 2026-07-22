/*==============================================================================================

    runtime_service/gui/gui_render.c -- GUI_RENDER translation unit: the RENDER SERVER.

    The 2d batch renderer (GUI_SERVER_PLAN.md): a narrow primitive foundation any 2d utility
    can emit to.  This unit owns the pixel pipeline: the CPU draw list, path stroking, the
    CPU tessellator, the GPU flush, and the debug overlay -- plus, until R3 moves them up to
    the draw unit, the font/icon resources (the server proper renders from a pushed atlas
    and does not know what a font is).  It produces no UI -- the layers above call the
    draw_* / font_* primitives declared in render/gui_render.h, and this unit turns that
    semantic command list into vertices and submits them.  It never sees the interact
    server: ids, widget state, and style stay above the seam.

    It does NOT define the module API pointer storage (MOD_USE_RHI / MOD_USE_APP): those globals
    live in gui.c and are fetched once at module init; this unit reads them through the same
    inline rhi() / app() accessors (extern g_*_api_ptr) from rhi_api.h / app_api.h.

    Include order matters: each file can reference statics from files included above it.  That
    order lives in the #include list below, not in the filenames.  Two subfolders name the two
    halves of the backend:

        resource/  -- GPU-backed assets with their own init/shutdown/query API (atlas, font,
                       icon), consumed BY the pipeline.  Each exposes a narrow query function the
                       pipeline reads from (font_atlas_idx, icon_atlas_idx, ...) rather than a
                       struct it reaches into, and never calls into pipeline/ itself.
        pipeline/  -- the per-frame submission path: EMIT (semantic draw list) -> BUILD
                       (tessellate + retained cache) -> RENDER (GPU flush).  Named for the
                       pipeline stage each implements, matching the function prefix each exports.

    resource/gui_atlas.h/.c         -- shared GPU-atlas asset: gui_atlas_t, gui_atlas_create/upload/destroy
    resource/gui_font.h             -- font types shared between the two font files below
    resource/gui_font_internal.c    -- font registry state + .orb_font loader (all static)
    resource/gui_font.c             -- font unit's public API: font_load/use, font_glyph (gui_render.h)
    resource/gui_icon.c             -- runtime icon atlas: icon_register/find/get, icon_atlas_idx
    resource/gui_icon_load.c        -- icon pixel sourcing: decode PNG/... -> R8 coverage -> register

    pipeline/gui_shader.h           -- embedded SPIR-V arrays (s_gui_vert_spirv, s_gui_frag_spirv)
    pipeline/gui_emit_draw.c        -- EMIT: CPU draw list: draw_reset, draw_push_* (incl. draw_push_icon), s_draw
    pipeline/gui_emit_path.c        -- EMIT: line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    pipeline/gui_build_tess.c       -- BUILD: CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    pipeline/gui_build_volatile.c   -- BUILD: volatile-widget inline-emit replay (see gui_render.h)
    pipeline/gui_build_cache.c      -- BUILD: retained frame-geometry cache: cache_build_frame, s_cache, s_dispatch,
                                        gui_build_* public API
    pipeline/gui_render.c           -- RENDER: GPU resources + flush: viewport_create/destroy, gui_render_* public API

    gui_debug_overlay.c             -- DEBUG OVERLAY: bolt-on second draw list, flushed on top (Debug only).  Stays
                                        at the render/ root -- it reads resource/ AND pipeline/ internals plus the
                                        UI unit's DBG_* capture calls, so it does not belong to either subfolder.
    gui_render_mem.c               -- MEMORY ACCOUNTING: gui_backend_memory sizeof-sums every backend static;
                                        must be included last so it sees them all.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"
#include "base/fmt.h"   // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting on the per-frame text paths

// Shared internal types + the render-backend interface (pulls gui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/gui/render/gui_render.h"

/*==============================================================================================
    Capability flags -- latched by gui_backend_init, read directly (same TU) by any file below
    that owns an optional layer: gui_icon.c (icons), gui_build_cache.c (retained_cache,
    stats_trace), gui_render.c (render_debug, stats_trace).  Declared before every include below
    so all of them see it, same visibility model as s_render / s_draw / s_tess.
==============================================================================================*/

static gui_backend_caps_t s_caps;

/*==============================================================================================
    Unity build
==============================================================================================*/

// resource/ -- foundation: the shared GPU-atlas helper, then the single shared resource atlas, then
// fonts + icons built on it.  gui_atlas.h/.c factors out the raw create/upload/destroy of one GPU
// texture; gui_res_atlas.h/.c owns THE shared R8 atlas (one texture, one bindless slot) that fonts
// and icons pack into as tenants so all core UI draws share tex_idx and batch together.
#include "runtime_service/gui/render/resource/gui_atlas.h"
#include "runtime_service/gui/render/resource/gui_atlas.c"
#include "runtime_service/gui/render/resource/gui_res_atlas.h"
#include "runtime_service/gui/render/resource/gui_res_atlas.c"
/* Fonts + icons moved to the draw unit (gui_draw.c, GUI_SERVER_PLAN.md R3) -- the server
   renders from the shared atlas they push into; glyph/icon UV lookups at tess/emit time go
   through the glyph/sprite source contract in gui_render.h. */

// pipeline/ -- types and embedded shader bytecode only, no logic.
#include "runtime_service/gui/render/pipeline/gui_shader.h"

// pipeline/ EMIT: the semantic draw list (s_draw) and the line/path stroker built on it.
// draw_push_icon lives here (not in resource/gui_icon.c): it queues a semantic command like
// every other draw_push_*, reading icon_get / icon_atlas_idx rather than the resource reaching
// up into EMIT itself.
#include "runtime_service/gui/render/pipeline/gui_emit_draw.c"
#include "runtime_service/gui/render/pipeline/gui_emit_path.c"

// pipeline/ BUILD, part A: tessellation primitives (gui_cmd_t -> s_tess geometry).
// No public surface -- driven entirely from part B (cache_tess_window / cache_build_frame).
#include "runtime_service/gui/render/pipeline/gui_build_tess.c"

// pipeline/ BUILD, part A.5: volatile widgets (inline-emit callback replay) -- see that file's header.
// After gui_build_tess.c (needs s_tess + tess_dispatch + s_volatile_patching); before
// gui_build_cache.c (defines the cache_slot_lookup / cache_invalidate_window /
// cache_count_volatile_patch helpers this file forward-declares).
#include "runtime_service/gui/render/pipeline/gui_build_volatile.c"

// pipeline/ BUILD, part B: retained cache & orchestration (diff, reuse-or-tessellate, z-sort).
#include "runtime_service/gui/render/pipeline/gui_build_cache.c"

// pipeline/ RENDER: GPU resource lifecycle + the per-surface flush.
#include "runtime_service/gui/render/pipeline/gui_submit.c"

// DEBUG OVERLAY: a parallel mini-pipeline, compiled out unless GUI_DEBUG_OVERLAY.  Stays at the
// render/ root -- see the file banner above for why.
#include "runtime_service/gui/render/gui_debug_overlay.c"

// PIPELINE DASHBOARD capture: snapshots the pipeline at the two capture points for the shell
// (gui_dashboard.c) to draw with the standard API.  Compiled out unless GUI_PIPELINE_DASHBOARD.
// Last so it sees every pipeline static it snapshots (s_draw, s_tess, the slot tables,
// s_volatile, s_tess_gen_next).
#include "runtime_service/gui/render/gui_dash_capture.c"

// TEXT-SELECTION run capture: copies flagged windows' text commands into a persistent run
// buffer at the build seam for the chrome unit's selection controller (chrome/window/gui_select.c).
// Always compiled (a product feature).  Last (with the captures below) so it sees s_draw.
#include "runtime_service/gui/render/gui_select_capture.c"

// COMMAND STEPPER capture + frozen-frame replay: snapshots the band-0 command list at the build
// seam and pre-loads it back at every draw_reset while frozen.  Compiled out unless
// GUI_CMD_STEPPER.  Last (with the dash capture) so it sees the emit statics it copies (s_draw).
#include "runtime_service/gui/render/gui_step_capture.c"

// MEMORY ACCOUNTING: sizeof-sums every backend static into the gui_mem_stats_t buckets.  MUST
// stay the very last include -- unity visibility only flows downward, and the full-accounting
// contract is that every static above is in scope here.
#include "runtime_service/gui/render/gui_render_mem.c"

/*==============================================================================================
    Backend lifecycle seam -- the entry point the UI unit (gui_init/gui_shutdown, gui_frame.c)
    calls, mirroring how gui.c fronts the UI unit.  Ties together whatever the backend needs to
    stand up as a whole; today that's just the RENDER stage's GPU resources, but it's the one
    place to add more later without the UI unit reaching into a stage-specific name.
==============================================================================================*/

bool
gui_backend_init( gui_backend_caps_t caps )
{
    s_caps = caps;

    if ( !gui_render_init() )   /* shared pipeline / sampler (gui_render.c) */
        return false;

    /* The shared resource atlas is core, not optional: fonts pack into it too, so it must exist
       before the host's first font_load.  One owned R8 texture + bindless slot; created here after
       the render pipeline (which owns the sampler) and before any font/icon registration. */
    if ( !res_atlas_init() )
    {
        gui_render_shutdown();
        return false;
    }

    /* Fonts and icons are the DRAW unit's resources now -- the frame orchestrator boots them
       right after this returns (gui_draw_boot), so they register into the atlas created above. */
    return true;
}

void
gui_backend_exit( void )
{
    res_atlas_shutdown();
    gui_render_shutdown();
}

/*============================================================================================*/
// clang-format on
