/*==============================================================================================

    runtime_service/gui/gui_render.c -- GUI_RENDER translation unit: the RENDER SERVER.

    The 2d batch renderer: a narrow primitive foundation any 2d utility
    can emit to.  This unit owns the pixel pipeline: the CPU draw list, path stroking, the
    CPU tessellator, the GPU flush, and the debug overlay.  It produces no UI -- the layers
    above call the draw_* primitives declared in render/gui_render.h, and this unit turns
    that semantic command list into vertices and submits them.  It never sees the interact
    server: ids, widget state, and style stay above the seam.  Nor does it know what a font
    or an icon IS: those resources are the draw unit's (gui_draw.c), one level up; the server
    only renders from the shared atlas they pack into and resolves their UVs through the
    glyph/sprite source contract in render/gui_render.h.

    It does NOT define the module API pointer storage (MOD_USE_RHI / MOD_USE_APP): those globals
    live in gui.c and are fetched once at module init; this unit reads them through the same
    inline rhi() / app() accessors (extern g_*_api_ptr) from rhi_api.h / app_api.h.

    Include order matters: each file can reference statics from files included above it.  That
    order lives in the #include list below, not in the filenames.  Two subfolders name the two
    halves of the backend:

        resource/  -- GPU-backed assets with their own init/shutdown/query API, consumed BY the
                       pipeline.  Each exposes a narrow query function the pipeline reads from
                       rather than a struct it reaches into, and never calls into pipeline/
                       itself.  Only the atlas pair lives here now; fonts and icons -- once
                       resource/ tenants -- moved up to the draw unit and pack in from outside.
        pipeline/  -- the per-frame submission path: EMIT (semantic draw list) -> BUILD
                       (tessellate + retained cache) -> RENDER (GPU flush).  Named for the
                       pipeline stage each implements, matching the function prefix each exports.

    Everything else sits at the render/ root: the debug overlay and the three CAPTURES.  A
    capture is the same shape each time -- it snapshots pipeline statics at a build seam for a
    consumer that must not reach in -- so they share the root rather than a subfolder of their
    own, and all trail the pipeline includes because a snapshot must see what it copies.

    resource/gui_atlas.h/.c         -- shared GPU-atlas asset: gui_atlas_t, gui_atlas_create/upload/destroy
    resource/gui_res_atlas.h/.c     -- THE shared R8 atlas (one texture, one bindless slot) fonts and
                                        icons pack into as tenants, so all core UI draws batch together

    pipeline/gui_shader.h           -- embedded SPIR-V arrays (s_gui_vert_spirv, s_gui_frag_spirv)
    pipeline/gui_emit_draw.c        -- EMIT: CPU draw list: draw_reset, draw_push_* (incl. draw_push_icon), s_draw
    pipeline/gui_emit_path.c        -- EMIT: line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    pipeline/gui_build_tess.c       -- BUILD: CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    pipeline/gui_build_volatile.c   -- BUILD: volatile-widget inline-emit replay (see gui_render.h)
    pipeline/gui_build_cache.c      -- BUILD: retained frame-geometry cache: cache_build_frame, s_cache, s_dispatch,
                                        the build_* seam
    pipeline/gui_submit.c           -- RENDER: GPU resources + flush: surface_geo_create/destroy, the
                                        gui_render_* public API (render_init/shutdown stay TU-local)

    gui_debug_overlay.c             -- DEBUG OVERLAY: bolt-on second draw list, flushed on top (Debug only).  Stays
                                        at the render/ root -- it reads resource/ AND pipeline/ internals plus the
                                        frontend's DBG_* capture calls, so it does not belong to either subfolder.
    gui_dash_capture.c              -- CAPTURE: pipeline snapshot for the dashboard shell (GUI_PIPELINE_DASHBOARD)
    gui_select_capture.c            -- CAPTURE: flagged windows' text runs, for chrome's selection controller
    gui_step_capture.c              -- CAPTURE: band-0 command list + the frozen-frame reload (GUI_CMD_STEPPER)
    gui_render_mem.c                -- MEMORY ACCOUNTING: backend_memory sizeof-sums every backend static;
                                        must be included last so it sees them all.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"
#include "base/fmt.h"   // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting on the per-frame text paths

/* This unit's world, and nothing above it (R11: the include list IS the dependency graph).
   THE RENDER SERVER sees the public gui types, the engine APIs, and its own header -- never
   the interact server or a library unit.  The debug header is the sanctioned severable
   instrumentation (this unit IMPLEMENTS the capture entry points it declares). */
#include "runtime_service/gui/render/gui_render.h"   /* pulls gui_host.h + rhi/app APIs */
#include "runtime_service/gui/debug/gui_debug.h"

// clang-format off
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
/* Fonts + icons live in the draw unit (gui_draw.c) -- the server
   renders from the shared atlas they push into; glyph/icon UV lookups at tess/emit time go
   through the glyph/sprite source contract in gui_render.h. */

// pipeline/ -- types and embedded shader bytecode only, no logic.
#include "runtime_service/gui/render/pipeline/gui_shader.h"

// pipeline/ EMIT: the semantic draw list (s_draw) and the line/path stroker built on it.
// draw_push_icon lives here rather than with the icon resource (the draw unit's now): it queues
// a semantic command like every other draw_push_*, resolving UVs through the sprite source
// contract instead of the resource reaching up into EMIT itself.
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
    Backend lifecycle seam -- the entry point the frame orchestrator (gui_init / gui_shutdown,
    frame/gui_frame_loop.c) calls.  Ties together whatever the backend needs to stand up as a
    whole; today that's just the RENDER stage's GPU resources, but it's the one place to add
    more later without a caller reaching into a stage-specific name.
==============================================================================================*/

bool
backend_init( void )
{
    if ( !render_init() )   /* shared pipeline / sampler (gui_render.c) */
        return false;

    /* The shared resource atlas is core, not optional: fonts pack into it too, so it must exist
       before the host's first font_load.  One owned R8 texture + bindless slot; created here after
       the render pipeline (which owns the sampler) and before any font/icon registration. */
    if ( !res_atlas_init() )
    {
        render_shutdown();
        return false;
    }

    /* Fonts and icons are the DRAW unit's resources now -- the frame orchestrator boots them
       right after this returns (gui_draw_boot), so they register into the atlas created above. */
    return true;
}

void
backend_exit( void )
{
    res_atlas_shutdown();
    render_shutdown();
}

/*============================================================================================*/
// clang-format on
