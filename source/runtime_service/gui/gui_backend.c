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
    resource/gui_font.c             -- font unit's public API: font_load/use, font_glyph (gui_backend.h)
    resource/gui_icon.c             -- runtime icon atlas: icon_register/find/get, icon_atlas_idx

    pipeline/gui_shader.h           -- embedded SPIR-V arrays (s_gui_vert_spirv, s_gui_frag_spirv)
    pipeline/gui_emit_draw.c        -- EMIT: CPU draw list: draw_reset, draw_push_* (incl. draw_push_icon), s_draw
    pipeline/gui_emit_path.c        -- EMIT: line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    pipeline/gui_build_tess.c       -- BUILD: CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    pipeline/gui_build_volatile.c   -- BUILD: volatile-widget inline-emit replay (see gui_backend.h)
    pipeline/gui_build_cache.c      -- BUILD: retained frame-geometry cache: cache_build_frame, s_cache, s_dispatch,
                                        gui_build_* public API
    pipeline/gui_render.c           -- RENDER: GPU resources + flush: viewport_create/destroy, gui_render_* public API

    gui_debug_overlay.c             -- DEBUG OVERLAY: bolt-on second draw list, flushed on top (Debug only).  Stays
                                        at the backend/ root -- it reads resource/ AND pipeline/ internals plus the
                                        UI unit's DBG_* capture calls, so it does not belong to either subfolder.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls gui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/gui/gui_backend.h"

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

// resource/ -- foundation: shared GPU-atlas helper, then fonts + icons built on it.
// Independent of each other; each owns its own atlas instance, CPU staging, and deferred-upload
// lifecycle -- gui_atlas.h/.c only factors out the create/upload/destroy sequence they share.
#include "runtime_service/gui/backend/resource/gui_atlas.h"
#include "runtime_service/gui/backend/resource/gui_atlas.c"
#include "runtime_service/gui/backend/resource/gui_font.h"
#include "runtime_service/gui/backend/resource/gui_font_internal.c"
#include "runtime_service/gui/backend/resource/gui_font.c"
#include "runtime_service/gui/backend/resource/gui_icon.c"

// pipeline/ -- types and embedded shader bytecode only, no logic.
#include "runtime_service/gui/backend/pipeline/gui_shader.h"

// pipeline/ EMIT: the semantic draw list (s_draw) and the line/path stroker built on it.
// draw_push_icon lives here (not in resource/gui_icon.c): it queues a semantic command like
// every other draw_push_*, reading icon_get / icon_atlas_idx rather than the resource reaching
// up into EMIT itself.
#include "runtime_service/gui/backend/pipeline/gui_emit_draw.c"
#include "runtime_service/gui/backend/pipeline/gui_emit_path.c"

// pipeline/ BUILD, part A: tessellation primitives (gui_cmd_t -> s_tess geometry).
// No public surface -- driven entirely from part B (cache_tess_window / cache_build_frame).
#include "runtime_service/gui/backend/pipeline/gui_build_tess.c"

// pipeline/ BUILD, part A.5: volatile widgets (inline-emit callback replay) -- see that file's header.
// After gui_build_tess.c (needs s_tess + tess_dispatch + s_volatile_patching); before
// gui_build_cache.c (defines the cache_slot_lookup / cache_invalidate_window /
// cache_count_volatile_patch helpers this file forward-declares).
#include "runtime_service/gui/backend/pipeline/gui_build_volatile.c"

// pipeline/ BUILD, part B: retained cache & orchestration (diff, reuse-or-tessellate, z-sort).
#include "runtime_service/gui/backend/pipeline/gui_build_cache.c"

// pipeline/ RENDER: GPU resource lifecycle + the per-surface flush.
#include "runtime_service/gui/backend/pipeline/gui_render.c"

// DEBUG OVERLAY: a parallel mini-pipeline, compiled out unless GUI_DEBUG_OVERLAY.  Stays at the
// backend/ root -- see the file banner above for why.
#include "runtime_service/gui/backend/gui_debug_overlay.c"

// PIPELINE DASHBOARD content: another parallel mini-pipeline (own snapshot + own vb/ib),
// compiled out unless GUI_PIPELINE_DASHBOARD.  Last so it sees every pipeline static it
// visualizes (s_draw, s_tess, the slot tables, s_volatile, s_render) plus gui_debug_name.
#include "runtime_service/gui/backend/gui_dash_overlay.c"

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

    if ( !gui_render_init() )   /* shared pipeline / sampler / atlas (gui_render.c) */
        return false;

    /* Icon atlas is an optional layer over the core font/render pipeline -- stood up here (not
       inside font_init) so a caller that never touches icons never pays for it. */
    if ( s_caps.icons && !icon_atlas_init() )
    {
        gui_render_shutdown();
        return false;
    }

    return true;
}

void
gui_backend_exit( void )
{
    if ( s_caps.icons )
        icon_atlas_shutdown();
    gui_render_shutdown();
}

/*============================================================================================*/
// clang-format on
