/*==============================================================================================

    runtime_service/gui/gui_render.c -- GUI_RENDER translation unit: the RENDER SERVER.

    --------------------------------------------------------------------------------------------
    Overview:

    The part of the GUI that puts pixels on the screen: a small 2d renderer -- rects, lines,
    text -- batched into QUAD RECORDS, with no idea what a "widget" is. Everything above it
    (layout, style, widgets) boils down to simple draw commands ("fill this rect", "draw this
    text"); this unit's only job is turning that command list into GPU-expandable records and
    submitting them each frame. It owns the whole pixel pipeline: collect the frame's draw
    commands, tessellate each shape into a 16-byte record (CPU-side), stroke lines and paths,
    flush to the GPU, and a debug overlay for watching it all run.

    It never sees the interact server -- no ids, no hover/click, input stays entirely above this
    layer -- and it does not know what a font or icon actually IS; that belongs to the draw unit
    one level up (gui_draw.c), which packs glyph/icon pixels into a shared atlas and tells this
    unit where to find them.

    Three subfolders name the backend's parts:

        resource:   The GPU atlases this unit owns.
        pipeline:   The per-frame EMIT -> BUILD -> RENDER path.
        utility:    The debug overlay, dashboard, text selection, frame stepper.

    Only gui_render.h sits at render/ root.

    --------------------------------------------------------------------------------------------
    Taxonomy (include order -- each file below sees statics from every file listed above it):

    resource/
     gui_atlas.h/.c            -- one GPU atlas texture: create/upload/destroy, either pixel format
     gui_res_atlas.h/.c        -- the three atlases over that primitive: R8 COVERAGE (fonts +
                                  icons share one tex_idx so both batch), RGBA SPRITE, SDF (latter two lazy)
    pipeline/
     gui_emit_state.c          -- EMIT: draw list state -- s_draw, draw_reset, clip stack, ambient
     gui_emit_cmd.c            -- EMIT: command record -- draw_cmd_claim/_open/_seal, draw_hash_cmd
     gui_emit_shape.c          -- EMIT: fills, pictures, gradients, draw_push_icon
     gui_emit_fx.c             -- EMIT: SDF surfaces -- shadows, sectors, patterns, lattices
     gui_emit_edge.c           -- EMIT: outlines, bezels, the bare triangle
     gui_emit_text.c           -- EMIT: glyph runs -- draw_push_text, _shadow, _xf
     gui_emit_path.c           -- EMIT: line/path stroking -- draw_line, draw_polyline, path_*
     gui_build_tess_state.c    -- BUILD: tessellator state -- s_tess arenas, ambient, overflow/dirty tracking
     gui_build_tess_quad.c     -- BUILD: quad-record core -- tess_quad_push, quantizers, style/fx dedup
     gui_build_tess_sprite.c   -- BUILD: nine-slice sprite expansion, hollow-rect outlines
     gui_build_tess_sdf.c      -- BUILD: rounded-box SDF family -- fx_box, repeat lattices, triangle/bezier
     gui_build_tess_arc.c      -- BUILD: circles, n-gons, round-rect corners, arcs/sectors
     gui_build_tess_text.c     -- BUILD: tiling patterns, glyph runs, dashed lines
     gui_build_tess_dispatch.c -- BUILD: polyline stroker + tess_dispatch, the command-type switch
     gui_build_volatile.c      -- BUILD: volatile-widget inline-emit replay (see gui_render.h)
     gui_build_cache.c         -- BUILD: retained-cache driver -- cache_build_frame sequences diff + place
     gui_build_diff.c          -- BUILD: change detection -- hashes each window's commands vs last frame
     gui_build_place.c         -- BUILD: per-window placement -- reuse cached geometry or retessellate
     gui_render_init.c         -- RENDER: shared GPU resources, created once -- pipeline, samplers, push-constants
     gui_render_pal.c          -- RENDER: the prim palette -- frame-global prim records past every arena region
     gui_render_intern.c       -- RENDER: what enters the palette -- lookup, interning, per-command memo, style epoch
     gui_render_submit.c       -- RENDER: per-surface GPU submit -- gui_render_flush, debug-mode/time setters

    utility/
     gui_prim_census.c         -- STYLE CENSUS: session-wide record-reuse histogram (GUI_PRIM_CENSUS only)
     gui_debug_overlay.c       -- DEBUG OVERLAY: second draw list, flushed on top (Debug only) --
                                  reads resource/ AND pipeline/ internals, which is why it can't live in either subfolder
     gui_dash_capture.c        -- CAPTURE: pipeline snapshot for the dashboard shell (GUI_PIPELINE_DASHBOARD)
     gui_select_capture.c      -- CAPTURE: flagged windows' text runs, for chrome's selection controller
     gui_step_capture.c        -- CAPTURE: band-0 command list + the frozen-frame reload (GUI_CMD_STEPPER)
     gui_render_mem.c          -- MEMORY ACCOUNTING: sums every backend static's sizeof.

    --------------------------------------------------------------------------------------------
    Frame Pipeline:

    EMIT (real frames only): widgets write shape lines to a list -- nothing draws yet.
    Idle frame: skipped, the previous frame's records are reused as-is.

    BUILD - diff: hash each window's list and compare to last frame; only changed windows
    proceed. Idle frame: skipped too.

    BUILD - tess: changed windows become 16-byte quad records (gui_quad_t) in one CPU
    arena, one record per shape, no vertex or index buffer. Unchanged windows keep their
    existing records untouched, exactly where they were. Each record's clip tag is a 
    permanent address (cache slot x 16 + which clip) into that window's fixed shelf, so 
    cached records stay correct forever.

    RENDER - flush (every presented frame): quad records -- the live arena span, copied into
    this frame's region of the global quad table (2 GPU-rotated regions, so every region 
    needs the full set, changed or not). Clip rects -- almost never; a shelf resends only if 
    its stale flag is set (max 512 B/shelf), so a stable frame ships zero clip bytes. 
    
    Push constants -- one per surface (matrix, samplers, clock, shelf-table base). Then one
    bufferless draw call per window, back to front (6*N vertices; the vertex stage expands 
    corners via SV_VertexID, the fragment stage clips by reading the shelf each quad names).
    Fully idle app: nothing at all.

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>     // offsetof -- the style census names gui_prim_t's lanes by offset
#include <stdarg.h>     // va_list  -- the census's bounded string append
#include <math.h>

#include "orb.h"
#include "base/fmt.h"   // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting for text paths
#include "base/utf8.h"  // codepoint stepping in the glyph-run tessellators + stepper AABB walks

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/gui/render/gui_render.h"      /* pulls gui_host.h + rhi/app APIs */
#include "runtime_service/gui/debug/gui_debug.h"

/*==============================================================================================
    GUI Resources : the texture atlases fonts, icons, sprites and SDF glyphs pack into.

    gui_atlas is the primitive: create/upload/destroy of one GPU texture, in either pixel
    format. gui_res_atlas is the policy layer on top of it -- it owns the three atlases
    (R8 coverage, RGBA sprite, SDF) as separate packers, each with its own texture and
    bindless slot, and assigns every tenant a tex_idx into the atlas it belongs to. 
    Since tex_idx rides the vertex, draws into the same atlas batch together for free.
==============================================================================================*/

#include "runtime_service/gui/render/resource/gui_atlas.h"
#include "runtime_service/gui/render/resource/gui_atlas.c"
#include "runtime_service/gui/render/resource/gui_res_atlas.h"
#include "runtime_service/gui/render/resource/gui_res_atlas.c"

/*  Fonts, icons and sprites live in the draw unit (gui_draw.c) -- the server
    renders from the atlases they push into; glyph/icon/sprite UV lookups at tess/emit time go
    through the glyph/sprite source contract in gui_render.h. */

/*==============================================================================================
    Pipeline Emit

    The semantic draw list (s_draw) and the pushes built on it, split by what each
    file emits.  The order is a dependency chain and not a preference: _state owns s_draw and
    the ambient every later file reads, _cmd owns the claim/hash/seal every push calls, and
    _shape defines draw_push_rect_filled / _outline, which the bezel, the disc, the icon and
    the trace all reach back for.  Unity visibility flows downward only.

    draw_push_icon lives here rather than with the icon resource (the draw unit's now): 
    it queues a semantic command like every other draw_push_*, resolving UVs through the 
    sprite source contract instead of the resource reaching up into EMIT itself. 
==============================================================================================*/

#include "runtime_service/gui/render/pipeline/gui_emit_state.c"
#include "runtime_service/gui/render/pipeline/gui_emit_cmd.c"
#include "runtime_service/gui/render/pipeline/gui_emit_shape.c"
#include "runtime_service/gui/render/pipeline/gui_emit_fx.c"
#include "runtime_service/gui/render/pipeline/gui_emit_edge.c"
#include "runtime_service/gui/render/pipeline/gui_emit_text.c"
#include "runtime_service/gui/render/pipeline/gui_emit_path.c"

/*==============================================================================================
    Style Census

    STYLE RECORD CENSUS: session-wide histogram of the records the tessellator emits,
    and of the arena entries each one costs across window slots.

    Before the gui_build_tess_*.c family because those files call its hooks; depends on
    nothing but the public gui types. Compiled out unless GUI_PRIM_CENSUS.

==============================================================================================*/

#include "runtime_service/gui/render/utility/gui_prim_census.c"

/*==============================================================================================
    Pipeline Build
==============================================================================================*/

/* tessellation primitives (gui_cmd_t -> s_tess geometry) */
#include "runtime_service/gui/render/pipeline/gui_build_tess_state.c"
#include "runtime_service/gui/render/pipeline/gui_build_tess_quad.c"
#include "runtime_service/gui/render/pipeline/gui_build_tess_sprite.c"
#include "runtime_service/gui/render/pipeline/gui_build_tess_sdf.c"
#include "runtime_service/gui/render/pipeline/gui_build_tess_arc.c"
#include "runtime_service/gui/render/pipeline/gui_build_tess_text.c"
#include "runtime_service/gui/render/pipeline/gui_build_tess_dispatch.c"

/* volatile widgets (inline-emit callback replay) */
#include "runtime_service/gui/render/pipeline/gui_build_volatile.c"

/* shared slot/stats state + the cache_build_frame driver that sequences */
#include "runtime_service/gui/render/pipeline/gui_build_cache.c"

/* change detection -- diffs this frame's command hashes against last frame */
#include "runtime_service/gui/render/pipeline/gui_build_diff.c"

/* per-window placement -- reuse or tessellate each window */
#include "runtime_service/gui/render/pipeline/gui_build_place.c"

/*==============================================================================================
    Pipeline Render
==============================================================================================*/

/* shared GPU resources (pipeline, samplers), created once */
#include "runtime_service/gui/render/pipeline/gui_render_init.c"

/* the prim palette -- shared prim records past every arena region. */
#include "runtime_service/gui/render/pipeline/gui_render_pal.c"

/* what goes IN the palette -- the lookup, interning, the per-command memo and the style epoch */
#include "runtime_service/gui/render/pipeline/gui_render_intern.c"

/* per-surface submit (gui_render_flush) */
#include "runtime_service/gui/render/pipeline/gui_render_submit.c"

/*==============================================================================================
    Utility: Debug / Capture / Memory Accounting
==============================================================================================*/

/* DEBUG OVERLAY: a parallel mini-pipeline, compiled out unless GUI_DEBUG_OVERLAY */
#include "runtime_service/gui/render/utility/gui_debug_overlay.c"

/* PIPELINE DASHBOARD capture: snapshots the pipeline at the two capture points for the shell */
#include "runtime_service/gui/render/utility/gui_dash_capture.c"

/* TEXT-SELECTION run capture: copies flagged windows' text commands into a persistent run */
#include "runtime_service/gui/render/utility/gui_select_capture.c"

/* COMMAND STEPPER capture + frozen-frame replay: snapshots and walks back to debug */
#include "runtime_service/gui/render/utility/gui_step_capture.c"

/* MEMORY ACCOUNTING: sizeof-sums every backend static into the gui_mem_stats_t buckets */
#include "runtime_service/gui/render/utility/gui_render_mem.c"

/*==============================================================================================
    Initialization

    Backend lifecycle seam -- the entry point the frame orchestrator calls. Ties together 
    whatever the backend needs to stand up as a whole; today that's just the RENDER stage's
    GPU resources, but it's the one place to add more later without a caller reaching into
    a stage-specific name.
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
