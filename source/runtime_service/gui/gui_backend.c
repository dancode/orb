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

    Include order matters: each file can reference statics from files included above it.  Tier
    2-6 files carry a numeric prefix so a directory listing sorts in pipeline order; Tier 1
    resource files (fonts/atlas/icon) don't -- they're registries the rest of the backend
    consumes all frame, not pipeline stages.

    gui_submit_shader.h     -- embedded SPIR-V arrays (s_gui_vert_spirv, s_gui_frag_spirv)
    gui_atlas.h/.c          -- shared GPU-atlas asset: gui_atlas_t, gui_atlas_create/upload/destroy
    gui_font.h              -- font types shared between the two font files below
    gui_font_internal.c     -- font registry state + .orb_font loader (all static)
    gui_font.c              -- font unit's public API: font_load/use, font_glyph (gui_backend.h)
    gui_icon.c              -- runtime icon atlas: icon_register/find/get, draw_push_icon
    gui_01_emit_draw.c      -- CPU draw list: draw_reset, draw_push_*, s_draw
    gui_01_emit_path.c      -- line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)
    gui_02_build_tess.c     -- CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    gui_02_build_cache.c    -- retained frame-geometry cache (BUILD): cache_build_frame, s_cache, s_dispatch
    gui_03_submit_render.c  -- GPU resources + flush (SUBMIT): viewport_create/destroy, init/shutdown/flush
    gui_04_debug_overlay.c  -- bolt-on debug overlay: separate draw list flushed on top (Debug only)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"

// Shared internal types + the render-backend interface (pulls gui_internal.h + rhi_api.h + app_api.h)
#include "runtime_service/gui/gui_backend.h"

/*==============================================================================================
    Capability flags -- latched by gui_backend_init, read directly (same TU) by any tier file
    below that owns an optional layer: gui_icon.c (icons), gui_02_build_cache.c (retained_cache,
    stats_trace), gui_03_submit_render.c (render_debug, stats_trace).  Declared before every tier
    include so all of them see it, same visibility model as s_render / s_draw / s_tess.
==============================================================================================*/

static gui_backend_caps_t s_caps;

/*==============================================================================================
    Unity build
==============================================================================================*/

// Tier 0 -- Foundation: types and embedded shader bytecode only, no logic.
#include "runtime_service/gui/backend/gui_submit_shader.h"

// Tier 1 -- Resource registries: fonts + icons, on a shared GPU-atlas helper (gui_atlas).
// Independent of each other; each owns its own atlas instance, CPU staging, and deferred-upload
// lifecycle -- gui_atlas.h/.c only factors out the create/upload/destroy sequence they share.
#include "runtime_service/gui/backend/gui_atlas.h"
#include "runtime_service/gui/backend/gui_atlas.c"
#include "runtime_service/gui/backend/gui_font.h"
#include "runtime_service/gui/backend/gui_font_internal.c"
#include "runtime_service/gui/backend/gui_font.c"
#include "runtime_service/gui/backend/gui_icon.c"

// Tier 2 -- EMIT: the semantic draw list (s_draw) and the line/path stroker built on it.
#include "runtime_service/gui/backend/gui_01_emit_draw.c"
#include "runtime_service/gui/backend/gui_01_emit_path.c"

// Tier 3 -- BUILD, part A: tessellation primitives (gui_cmd_t -> s_tess geometry).
// No public surface -- driven entirely from Tier 4 (cache_tess_window / cache_build_frame).
#include "runtime_service/gui/backend/gui_02_build_tess.c"

// Tier 4 -- BUILD, part B: retained cache & orchestration (diff, reuse-or-tessellate, z-sort).
#include "runtime_service/gui/backend/gui_02_build_cache.c"

// Tier 5 -- SUBMIT: GPU resource lifecycle + the per-surface flush.
#include "runtime_service/gui/backend/gui_03_submit_render.c"

// Tier 6 -- Debug overlay: a parallel mini-pipeline, compiled out unless GUI_DEBUG_OVERLAY.
#include "runtime_service/gui/backend/gui_04_debug_overlay.c"

/*==============================================================================================
    Backend lifecycle seam -- the entry point the UI unit (gui_init/gui_shutdown, gui_frame.c)
    calls, mirroring how gui.c fronts the UI unit.  Ties together whatever the backend needs to
    stand up as a whole; today that's just the Tier 5 GPU resources, but it's the one place to
    add more later without the UI unit reaching into a tier-specific name.
==============================================================================================*/

bool
gui_backend_init( gui_backend_caps_t caps )
{
    s_caps = caps;

    if ( !gui_render_init() )   /* shared pipeline / sampler / atlas (gui_03_submit_render.c) */
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
