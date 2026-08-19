/*==============================================================================================

    runtime_service/gui/gui_render.c -- GUI_RENDER translation unit: the RENDER SERVER.

    ------------------------------------------------------------------------------------------------
    Overview:

    This is the part of the GUI that actually puts pixels on the screen. It is a small,
    general-purpose 2d renderer -- rectangles, lines, and text turned into batches of QUAD
    RECORDS -- that has no idea what a "widget" is. Everything above it (layout, style, widgets)
    eventually boils down to a handful of simple draw commands ("fill this rect," "draw this
    line of text"), and this unit's only job is to turn that command list into records the GPU
    can expand, and submit them each frame.

    It owns the whole pixel pipeline: collecting the draw commands for a frame, turning each
    shape into one 16-byte record (the CPU tessellator), stroking lines and paths, flushing
    everything to the GPU, and a debug overlay for watching all of that while it runs.

    Two things this unit deliberately does not know about. First, it never sees the interact
    server -- no ids, no widget state, no hover or click -- input and interaction stay entirely
    above this layer. Second, it does not know what a font or an icon actually IS; that
    knowledge belongs to the draw unit one level up (gui_draw.c), which packs glyph and icon
    pixels into a shared texture and tells this unit where in that texture to find them.

    Include order matters: each file can reference statics from files included above it. That
    order lives in the #include list below, not in the filenames. Two subfolders name the two
    halves of the backend:

        resource/  -- the GPU-backed textures this unit owns (the shared atlases), each with
                       its own init/shutdown/query functions that the pipeline below reads
                       from, never reaching into pipeline/ itself.

        pipeline/  -- the per-frame path a draw command takes: EMIT (collect the command list)
                       -> BUILD (turn it into quad records, reusing cached geometry where nothing
                       changed) -> RENDER (send it to the GPU). Named for the stage each file
                       implements.

    Everything else sits at the render/ root: the debug overlay, and three "capture" files that
    each snapshot the pipeline's internal state for some outside consumer -- the debug
    dashboard, text selection, the frame stepper -- that is not allowed to reach into the
    pipeline directly.
    
    ------------------------------------------------------------------------------------------------
    Resource:

    resource/gui_atlas.h/.c         -- shared GPU-atlas asset: gui_atlas_t, gui_atlas_create/upload/destroy
    resource/gui_res_atlas.h/.c     -- the THREE resource atlases over one packer: the R8 COVERAGE
                                        atlas (one texture, one bindless slot) fonts and icons pack
                                        into so all core UI draws batch together, the RGBA SPRITE
                                        atlas for authored art, and the SDF atlas for distance-field
                                        glyphs -- the latter two created lazily, on first use
    ------------------------------------------------------------------------------------------------
    Pipeline:

    pipeline/gui_emit_draw.c        -- EMIT: CPU draw list: draw_reset, draw_push_* (incl. draw_push_icon), s_draw
    pipeline/gui_emit_path.c        -- EMIT: line / path stroking: draw_line, draw_polyline, path_* (uses s_draw)    
    pipeline/gui_build_tess.c       -- BUILD: CPU tessellation engine: s_tess, tess_reset, tess_dispatch, tess_* helpers
    pipeline/gui_build_volatile.c   -- BUILD: volatile-widget inline-emit replay (see gui_render.h)
    pipeline/gui_build_cache.c      -- BUILD: retained frame-geometry cache: cache_build_frame, s_cache, s_dispatch, the build_* seam.    
    pipeline/gui_render_init.c      -- RENDER: shared GPU resources, created once: pipeline, samplers,
                                        the push-constant layout (render_init/shutdown, TU-local)
    pipeline/gui_render_submit.c    -- RENDER: per-surface GPU submit: gui_render_flush, the
                                        debug-mode/time setters
    ------------------------------------------------------------------------------------------------
    Utility:

    gui_debug_overlay.c             -- DEBUG OVERLAY: bolt-on second draw list, flushed on top (Debug only).  Stays
                                        at the render/ root -- it reads resource/ AND pipeline/ internals plus the
                                        frontend's DBG_* capture calls, so it does not belong to either subfolder.

    gui_dash_capture.c              -- CAPTURE: pipeline snapshot for the dashboard shell (GUI_PIPELINE_DASHBOARD)
    gui_select_capture.c            -- CAPTURE: flagged windows' text runs, for chrome's selection controller
    gui_step_capture.c              -- CAPTURE: band-0 command list + the frozen-frame reload (GUI_CMD_STEPPER)

    gui_render_mem.c                -- MEMORY ACCOUNTING: backend_memory sizeof-sums every backend static;
                                        must be included last so it sees them all.
    ------------------------------------------------------------------------------------------------
    Frame Overview:

    Step 1 -- Write the shopping list (EMIT).

    * Every widget you call (button, text, ...) doesn't draw anything.
    * It just writes a line on a list: "rectangle here, this color", "text there, clipped to this".
    * This is essentially a list of shapes.
    * This only happens on a real frame, but if you didn't touch anything and nothing animated,
      we don't even write the list, we just reuse the previous frame's records.

    Step 2 -- Compare with the previous shape list (BUILD: diff).

    * For each window we hash its lines and compare with last frame. 
      "Same as before? Great, don't redo your work."
    * Only windows whose list actually changed go to step 3. 
    * On a totally idle frame, this whole step is skipped too!

    Step 3 -- Turn the shapes into quad records (BUILD: tessellate).

    * Changed windows get turned into 16-byte quad records (gui_quad_t) in one big CPU-side
      arena -- one record per shape, no vertex buffer and no index buffer anywhere.
    * Unchanged windows keep the records they already had, sitting exactly where they were
      last frame -- nothing moves, nothing is repacked.
    * Each record carries its own clip tag: "clip me with rect #N".
    * N is a permanent address: this window's fixed shelf in the clip cupboard
      (its cache slot x 16 + which clip).
    * Because the address is permanent, cached records stay correct forever.

    Step 4 -- Ship it (RENDER: flush, every presented frame).

    * This is the part that talks to the GPU, and here's exactly what gets uploaded now:
    * Quad records: the live span of the arena, copied into this frame's region of the global
      quad table. The GPU rotates between 2 regions, so each frame's region must contain
      everything, changed or not.
    * Clip rects: usually nothing at all. Each window's clips live on its fixed shelf in the GPU cupboard.
      A shelf is re-sent only if a little "stale" flag says its contents changed (max 512 bytes per shelf).
      Stable frame = zero clip bytes.
    * Push constants: one single push per surface -- matrix, samplers, clock, and "the cupboard starts here".

    * Then one bufferless draw call per window, back to front: cmd_draw of 6 * N bare vertices;
      the vertex stage pulls each record by SV_VertexID and expands the corners itself, and the
      fragment shader does the clipping by reading the shelf each quad named.

    * So per frame: quad records always (that's the frames-in-flight tax), clips almost never,
      push constants once. And if the app is fully idle and nothing presents -- nothing at all.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>   // offsetof -- the style census names gui_prim_t's lanes by offset
#include <stdarg.h>   // va_list  -- the census's bounded string append
#include <math.h>

#include "orb.h"
#include "base/fmt.h"   // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting on the per-frame text paths
#include "base/utf8.h"  // codepoint stepping in the glyph-run tessellators + stepper AABB walks

/* This unit's world, and nothing above it (R11: the include list IS the dependency graph).
   THE RENDER SERVER sees the public gui types, the engine APIs, and its own header -- never
   the interact server or a library unit.  The debug header is the sanctioned severable
   instrumentation (this unit IMPLEMENTS the capture entry points it declares). */

#include "runtime_service/gui/render/gui_render.h"      /* pulls gui_host.h + rhi/app APIs */
#include "runtime_service/gui/debug/gui_debug.h"

// clang-format off
/*==============================================================================================
    Unity build
==============================================================================================*/

// resource/ -- foundation: the GPU-atlas helper, then the resource atlases, then fonts + icons +
// sprites built on them.  gui_atlas.h/.c factors out the raw create/upload/destroy of one GPU
// texture at either pixel format; gui_res_atlas.h/.c owns both atlases (one texture and one
// bindless slot each) that fonts, icons and sprites pack into as tenants, so everything of a kind
// resolves to one tex_idx -- and since that word rides the vertex, the kinds batch together too.

#include "runtime_service/gui/render/resource/gui_atlas.h"
#include "runtime_service/gui/render/resource/gui_atlas.c"
#include "runtime_service/gui/render/resource/gui_res_atlas.h"
#include "runtime_service/gui/render/resource/gui_res_atlas.c"

/* Fonts, icons and sprites live in the draw unit (gui_draw.c) -- the server
   renders from the atlases they push into; glyph/icon/sprite UV lookups at tess/emit time go
   through the glyph/sprite source contract in gui_render.h. */

// pipeline/ EMIT: the semantic draw list (s_draw) and the line/path stroker built on it.
// draw_push_icon lives here rather than with the icon resource (the draw unit's now): it queues
// a semantic command like every other draw_push_*, resolving UVs through the sprite source
// contract instead of the resource reaching up into EMIT itself.
#include "runtime_service/gui/render/pipeline/gui_emit_draw.c"
#include "runtime_service/gui/render/pipeline/gui_emit_path.c"

// STYLE RECORD CENSUS: session-wide histogram of the records the tessellator emits, and of the
// arena entries each one costs across window slots.  Before gui_build_tess.c because that file
// calls its hooks; depends on nothing but the public gui types.  Compiled out unless
// GUI_PRIM_CENSUS.
#include "runtime_service/gui/render/gui_prim_census.c"

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

// pipeline/ RENDER, part A: shared GPU resources (pipeline, samplers), created once.
#include "runtime_service/gui/render/pipeline/gui_render_init.c"

// pipeline/ RENDER, part B: the style palette -- shared records past every arena region.  After
// gui_render_init.c because it writes into that unit's prim_buf and region layout.
#include "runtime_service/gui/render/pipeline/gui_render_pal.c"

// pipeline/ RENDER, part B2: what goes IN the palette.  After gui_render_pal.c (it publishes into
// that unit) and after gui_build_tess.c (its rows run the real emitters); the placement pass calls
// it through the prototype in gui_render.h.
#include "runtime_service/gui/render/pipeline/gui_render_bake.c"

// pipeline/ RENDER, part C: per-surface submit (gui_render_flush).
#include "runtime_service/gui/render/pipeline/gui_render_submit.c"

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
