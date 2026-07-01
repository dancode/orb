/*==============================================================================================

    runtime_service/gui/backend/gui_build_cache.c -- Retained frame-geometry cache (BUILD phase).

    The render pipeline has three phases.  This file is the middle one:

        EMIT   (gui_emit_draw.c)    widgets push semantic shapes -> s_draw command list,
                                      cut into per-(win,z,vp) segments, one hash baked per command.
        BUILD  (this file)          once per frame: diff each window's commands against last frame,
                                      reuse unchanged geometry in place, tessellate changed windows,
                                      then z-sort the result into a dispatch table.
        SUBMIT (gui_submit_render.c) once per surface: upload changed geometry and emit one indexed
                                      draw call per cached GPU command.

    BUILD runs lazily on the first surface flush (cache_build_frame, guarded by s_frame_built)
    because the semantic command list is shared across every surface -- the geometry it produces
    is surface-independent.  gui_render_frame_reset clears the guard at frame_begin.

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"
// clang-format off

/*----------------------------------------------------------------------------------------------
    Once-per-frame guard.

    The first surface flush triggers cache_build_frame and stamps s_frame_built; later surfaces
    reuse the slot and dispatch tables untouched.
----------------------------------------------------------------------------------------------*/

static bool s_frame_built;

void
gui_render_frame_reset( void )
{
    s_frame_built = false;
}

/*----------------------------------------------------------------------------------------------
    Per-frame render stats.

    accum is built during BUILD (geometry/retained counts) and SUBMIT (draw_calls, summed across
    surfaces).  published is promoted from accum at frame_begin so the perf overlay always reads
    a stable snapshot one frame behind the geometry it describes.
----------------------------------------------------------------------------------------------*/

static struct
{
    gui_render_stats_t accum;        // built across this frame's BUILD + flush(es)
    gui_render_stats_t published;    // last frame's completed totals (what the overlay reads)
    u32                draw_call_hwm; // peak indexed draws in any single frame (lifetime)

} s_stats;

gui_render_stats_t
gui_render_stats( void )
{
    return s_stats.published;
}

void
gui_render_stats_publish( void )
{
    s_stats.published = s_stats.accum;
    s_stats.accum     = ( gui_render_stats_t ){ 0 };
}

// Peak draw-call count, read by the shutdown report in gui_submit_render.c.
static u32
cache_draw_call_hwm( void )
{
    return s_stats.draw_call_hwm;
}

// Fold one surface's draw-call count into the frame accumulator + lifetime peak (called by flush).
static void
cache_count_draw_calls( u32 draw_calls )
{
    s_stats.accum.draw_calls += draw_calls;
    if ( draw_calls > s_stats.draw_call_hwm )
        s_stats.draw_call_hwm = draw_calls;
}

// Fold one surface's upload batch/byte count into the frame accumulator (called by flush).
static void
cache_count_upload( u32 batches, u32 bytes )
{
    s_stats.accum.upload_batches += batches;
    s_stats.accum.upload_bytes   += bytes;
}

/*----------------------------------------------------------------------------------------------
    Retained-skip toggle.

    When enabled (default) a window whose per-command hash matches last frame keeps its geometry
    in place instead of re-tessellating.  Disable to benchmark or verify the from-scratch path.
    Toggled via gui()->set_retained_skip (key C in sb_vulkan).
----------------------------------------------------------------------------------------------*/

static bool s_retained_skip = true;

void gui_render_set_retained_skip( bool on ) { s_retained_skip = on; }
bool gui_render_retained_skip    ( void )    { return s_retained_skip; }

/*----------------------------------------------------------------------------------------------
    Build-phase debug toggles.

    Each prints a per-frame line only when its value changes, so a steady UI does not spam.
    The same numbers are live through gui()->render_stats() in the perf overlay.
----------------------------------------------------------------------------------------------*/

static bool s_dbg_cache    = false;  // per-window cache diff: unchanged count vs total
static bool s_dbg_geometry = false;  // emitted vertex / index counts (render density)
static bool s_dbg_retained = false;  // windows + verts/tris reused from last frame vs total
static bool s_exempt_perf_overlay = true;  // exclude the perf overlay window from stats totals

/*==============================================================================================
    Window geometry slots -- the retained geometry store.

    Each window owns one slot that caches its tessellated geometry: a vertex span, an index
    span, and the GPU draw commands that replay them.  An unchanged window replays its commands
    from the stable cache instead of re-tessellating; a changed window tessellates into its slot.
    Slots are z-sorted into the dispatch table that SUBMIT walks.

    Slot tables ping-pong between two backing stores (s_slots_a / s_slots_b) so this frame can
    read last frame's geometry positions (s_slots_prev) while building this frame's (s_slots).

    No separate prev-geometry buffer: s_tess.verts/indices persist between frames, so each
    window's geometry remains at its prev->vert_base until overwritten.  The reuse path advances
    the write head by vert_alloc (not vert_count) to keep the SLOT_VERT_PAD gap intact, which
    absorbs minor in-place growth without touching adjacent slots.
==============================================================================================*/

#define RENDER_MAX_WIN    32    // distinct windows tracked per frame
#define WIN_SLOT_CMD_MAX  16    // max GPU draw commands cached per slot; most windows have 2-4
#define SLOT_VERT_PAD     64u   // per-slot vertex headroom: absorbs minor growth in-place
#define SLOT_IDX_PAD      128u  // per-slot index headroom (~2x vertex count for quads)

/* One cached GPU draw command.  Packed AOS so replaying a slot's commands touches one region.
   z is per-slot (the window's max segment z), not per-command; lvbase is slot-local so the
   reuse path needs no fixup when the slot's absolute vert_base is unchanged. */
typedef struct
{
    gui_gpu_cmd_t cmd;     // clip rect, texture slot, element count
    u32           vp;      // viewport this command targets
    u32           lvbase;  // vertex base relative to slot->vert_base (0-relative)

} win_slot_cmd_t;

/* One window's cached geometry position and the command range that replays it. */
typedef struct
{
    gui_id_t win;
    u32      z, vp;
    u32      vert_base, vert_count, vert_alloc;  // VB: absolute position, actual count, padded reservation
    u32      idx_base,  idx_count,  idx_alloc;   // IB: absolute position, actual count, padded reservation
    u32      cmd_base,  cmd_count;               // range into s_tess.cmds[] for this window
    bool     valid;                              // true once geometry has been tessellated at least once

} win_geo_slot_t;

/* Stable GPU command cache -- outside the ping-pong arrays so the reuse path never copies it.
   Indexed by slot position wi, valid in set_stable mode (same window set and order as prev frame).
   Written when a window tessellates; read every retained frame until the window changes again. */
static win_slot_cmd_t s_win_cached      [ RENDER_MAX_WIN ][ WIN_SLOT_CMD_MAX ];
static u32            s_win_cached_count[ RENDER_MAX_WIN ];

static u32             s_slot_count, s_slot_prev_count;
static win_geo_slot_t  s_slots_a  [ RENDER_MAX_WIN ];
static win_geo_slot_t  s_slots_b  [ RENDER_MAX_WIN ];
static win_geo_slot_t* s_slots      = s_slots_a;   // current frame (write)
static win_geo_slot_t* s_slots_prev = s_slots_b;   // previous frame (read)
static win_geo_slot_t* s_dispatch [ RENDER_MAX_WIN ];
static u32             s_dispatch_count;

/*==============================================================================================
    Change detection (BUILD step 1) -- diff each window's commands against last frame.

    Each frame accumulates every window's per-command hashes (baked at emit by draw_hash_cmd)
    into one per-window hash, sorts by window id, then compares against last frame's sorted table
    in one linear scan.  A window whose hash matches is unchanged and may reuse its geometry.
    any_changed is the coarse signal that at least one window appeared, vanished, or changed.

    Hashing goes deep for TEXT and POLYLINE commands (which point into side pools): a
    same-length edit leaves the command struct byte-identical, so pool bytes are folded in too.
==============================================================================================*/

/* Per-window diff record.  cur[] is rebuilt each frame; prev[] is last frame's snapshot.
   z/vp are accumulated (max-z, last-vp across segments) so the slot builder needs no rescan. */
typedef struct
{
    gui_id_t win;
    u32      hash;
    u32      z;        // max segment z this frame (governs slot dispatch order)
    u32      vp;       // viewport of the last segment this frame
    bool     changed;  // hash mismatched or window is new this frame

} render_win_hash_t;

static struct
{
    render_win_hash_t cur [ RENDER_MAX_WIN ];  // this frame's per-window records
    render_win_hash_t prev[ RENDER_MAX_WIN ];  // last frame's hashes, for the diff

    u32  cur_n, prev_n;   // valid entries in cur[] / prev[]
    u32  unchanged;       // windows whose hash matched last frame
    bool any_changed;     // at least one window appeared, vanished, or changed

} s_cache = { .any_changed = true };   // start true so the first frame always builds

/* True when the PREVIOUS frame produced any render change.  Read by frame_begin before this
   frame's cache_build_frame runs; s_cache.any_changed still holds last frame's result then.
   When false on a frame with no input and no animation, the host may skip the widget emit. */
bool
gui_render_any_changed( void )
{
    return s_cache.any_changed;
}

/* cache_diff_windows -- accumulate per-window hashes from the command list, sort, and diff.
   Runs before tessellation so a fully-unchanged frame can skip tess entirely. */
static void
cache_diff_windows( void )
{
    const gui_cmd_seg_t* segs = s_draw.segs;
    u32                  nseg = s_draw.seg_count;

    /* Pass 1: fold each segment's command hashes into its window's accumulated hash.
       Also track max-z and last-vp per window so the slot builder needs no second scan.
       cmd_count is summed here to avoid a separate pass over segs later. */
    s_cache.cur_n = 0;
    u32 total_cmd = 0;
    for ( u32 si = 0; si < nseg; ++si )
    {
        if ( segs[ si ].lo == segs[ si ].hi ) continue;   // empty span

        gui_id_t win = segs[ si ].win;

        if ( !( s_exempt_perf_overlay && win == g_gui_perf_overlay_id ) )
            total_cmd += segs[ si ].hi - segs[ si ].lo;

        /* Find or create the per-window record. */
        u32 bi = 0;
        for ( ; bi < s_cache.cur_n; ++bi )
            if ( s_cache.cur[ bi ].win == win ) break;
        if ( bi == s_cache.cur_n )
        {
            if ( s_cache.cur_n >= RENDER_MAX_WIN ) continue;   // overflow: this window treated as changed
            s_cache.cur[ bi ] = ( render_win_hash_t ){ win, 2166136261u, 0, 0, false };
            ++s_cache.cur_n;
        }

        /* Fold z, vp, font, and atlas index into the hash.  The font id alone is not enough:
           font_load_into() can swap a different atlas under the same id, leaving the id stable
           while cached geometry references a retired atlas slot.  Folding the live atlas index
           catches this and forces re-tessellation. */
        u32 atlas = font_slot_atlas_idx( segs[ si ].font );
        u32 h     = s_cache.cur[ bi ].hash;
        h = fnv1a_u32( h, segs[ si ].z    );
        h = fnv1a_u32( h, segs[ si ].vp   );
        h = fnv1a_u32( h, segs[ si ].font );
        h = fnv1a_u32( h, atlas            );
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
            h = fnv1a_u32( h, s_draw.cmd_hashes[ i ] );
        s_cache.cur[ bi ].hash = h;

        if ( segs[ si ].z > s_cache.cur[ bi ].z ) s_cache.cur[ bi ].z = segs[ si ].z;
        s_cache.cur[ bi ].vp = segs[ si ].vp;
    }

    /* Sort cur[] by win id.  Insertion sort over RENDER_MAX_WIN = 32 elements: O(n) when the
       window set is stable (the common case, already sorted from last frame), O(n^2) at worst.
       prev[] is kept in the same order via the memcpy below, so the diff is a single linear scan. */
    for ( u32 a = 1; a < s_cache.cur_n; ++a )
    {
        render_win_hash_t key = s_cache.cur[ a ];
        u32 b = a;
        while ( b > 0 && s_cache.cur[ b - 1 ].win > key.win )
        {
            s_cache.cur[ b ] = s_cache.cur[ b - 1 ];
            --b;
        }
        s_cache.cur[ b ] = key;
    }

    /* Pass 2: diff against last frame.  Both arrays are sorted by win, so one linear scan
       suffices -- O(cur_n + prev_n) instead of the O(n^2) nested scan. */
    s_cache.unchanged   = 0;
    s_cache.any_changed = ( s_cache.cur_n != s_cache.prev_n );
    u32 pj = 0;
    for ( u32 i = 0; i < s_cache.cur_n; ++i )
    {
        while ( pj < s_cache.prev_n && s_cache.prev[ pj ].win < s_cache.cur[ i ].win )
            ++pj;
        bool match = ( pj < s_cache.prev_n
                       && s_cache.prev[ pj ].win  == s_cache.cur[ i ].win
                       && s_cache.prev[ pj ].hash == s_cache.cur[ i ].hash );
        s_cache.cur[ i ].changed = !match;
        if ( match ) ++s_cache.unchanged;
        else         s_cache.any_changed = true;
    }

    /* Promote this frame's sorted table to prev for next frame's diff. */
    memcpy( s_cache.prev, s_cache.cur, s_cache.cur_n * sizeof( render_win_hash_t ) );
    s_cache.prev_n = s_cache.cur_n;

    s_stats.accum.cmd_count = total_cmd;

    if ( s_dbg_cache && s_cache.any_changed )
        printf( "[gui] cache: %u/%u windows unchanged\n", s_cache.unchanged, s_cache.cur_n );
}

/*==============================================================================================
    Per-window tessellation (BUILD step 2 helper).

    Gathers a window's segments from s_draw.segs and builds a clip-sorted permutation, then
    hands it to tess_dispatch.  Grouping by clip makes equal-clip shapes tessellate back-to-back
    so they can merge into one GPU draw call.

    z is NOT sorted here -- a window occupies one slot whose dispatch z is its max segment z,
    keeping all of a window's geometry contiguous; cache_build_frame z-sorts the slots after.
==============================================================================================*/

#define RENDER_MAX_CLIP_GROUPS  64   // distinct clip indices groupable within one window

static void
cache_tess_window( gui_id_t win )
{
    const gui_cmd_seg_t* segs = s_draw.segs;
    u32                  nseg = s_draw.seg_count;

    /* Collect the distinct clip indices this window uses, in first-seen order.  Comparing u8
       indices is a single test vs four floats; the actual rect lives in s_draw.clip_table and
       is resolved at tessellation time inside tess_dispatch. */
    u8   clip_ids[ RENDER_MAX_CLIP_GROUPS ];
    u32  n_clips  = 0;
    bool overflow = false;

    for ( u32 si = 0; si < nseg && !overflow; ++si )
    {
        if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            u8 ci = s_draw.cmds[ i ].clip_idx;
            if ( rect_empty( s_draw.clip_table[ ci ] ) ) continue;
            bool seen = false;
            for ( u32 g = 0; g < n_clips; ++g )
                if ( clip_ids[ g ] == ci ) { seen = true; break; }
            if ( !seen )
            {
                if ( n_clips >= RENDER_MAX_CLIP_GROUPS ) { overflow = true; break; }
                clip_ids[ n_clips++ ] = ci;
            }
        }
    }

    /* Build the clip-sorted command permutation.  win_font[] carries the segment's font alongside
       each reordered index because the clip sort crosses segment boundaries -- the font is a
       per-segment property, not per-command, so it must travel with its commands.
       Static: cache_build_frame is single-threaded and guarded against re-entry. */
    static u32 win_order[ GUI_MAX_CMDS ];
    static u32 win_font [ GUI_MAX_CMDS ];
    u32 n = 0;

    if ( overflow )
    {
        /* Too many distinct clips: emit in natural order (correct, just less merged). */
        for ( u32 si = 0; si < nseg; ++si )
        {
            if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
            for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
                    { win_font[ n ] = segs[ si ].font; win_order[ n++ ] = i; }
        }
    }
    else
    {
        for ( u32 g = 0; g < n_clips; ++g )
            for ( u32 si = 0; si < nseg; ++si )
            {
                if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
                for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                    if ( s_draw.cmds[ i ].clip_idx == clip_ids[ g ] )
                        { win_font[ n ] = segs[ si ].font; win_order[ n++ ] = i; }
            }
    }

    tess_dispatch( s_draw.cmds, win_order, win_font, n );
}

/*==============================================================================================
    cache_build_frame (BUILD step 2) -- diff, reuse or re-tessellate per window, z-sort.

    Runs once per frame (guarded by s_frame_built).  Produces the geometry (s_tess.verts/indices),
    the per-window slot table, and the back-to-front dispatch order -- all surface-independent.

    In-place geometry reuse (set_stable mode):
      When the window set is identical to last frame (same windows, same sorted order, all slots
      valid), unchanged geometry sits in s_tess at its prev->vert_base.  The write head advances
      by vert_alloc (not vert_count), preserving the SLOT_VERT_PAD gap so the next window's
      prev->vert_base aligns with s_tess.vert_count -- no memmove ever needed.  Changed windows
      tessellate at that same write head, overwriting only their own prior region.

      If a changed window grows beyond its prev->vert_alloc, the alloc is expanded and all
      downstream prev slots are invalidated so they re-tessellate at their shifted positions.
      With SLOT_VERT_PAD=64 this triggers only when a window gains more than ~16 rects at once.
==============================================================================================*/

static void
cache_build_frame( void )
{
    if ( s_frame_built )
        return;
    s_frame_built = true;

    /* Close the still-open final segment so diff and tess see its full [lo, hi) range. */
    if ( s_draw.seg_count > 0 )
        s_draw.segs[ s_draw.seg_count - 1 ].hi = s_draw.cmd_count;

    /* Step 1: hash-diff all windows, fill s_cache, accumulate cmd_count stats. */
    cache_diff_windows();

    /* Rotate slot tables: last frame's slots become prev; build fresh into current. */
    win_geo_slot_t* tmp = s_slots_prev;
    s_slots_prev      = s_slots;
    s_slots           = tmp;
    s_slot_prev_count = s_slot_count;
    s_slot_count      = 0;
    s_dispatch_count  = 0;

    tess_reset();

    /* set_stable: the window set is the same as last frame (count, order, all valid).
       When true, each cur[wi] aligns with slots_prev[wi] by the sorted-by-win invariant,
       and unchanged geometry is in-place at prev->vert_base. */
    bool set_stable = ( s_slot_prev_count == s_cache.cur_n );
    for ( u32 wi = 0; set_stable && wi < s_cache.cur_n; ++wi )
        if ( s_slots_prev[ wi ].win != s_cache.cur[ wi ].win || !s_slots_prev[ wi ].valid )
            set_stable = false;

    u32 vert_retained = 0, tri_retained = 0, win_retained = 0;
    u32 total_vert    = 0, total_tri    = 0, overlay_win  = 0;

    /* Step 2: for each window, reuse geometry or re-tessellate, then register in dispatch. */
    for ( u32 wi = 0; wi < s_cache.cur_n; ++wi )
    {
        const render_win_hash_t* wh   = &s_cache.cur[ wi ];
        win_geo_slot_t*          slot = &s_slots[ s_slot_count++ ];
        win_geo_slot_t*          prev = set_stable ? &s_slots_prev[ wi ] : NULL;

        slot->win   = wh->win;
        slot->z     = wh->z;    // max segment z, pre-computed in cache_diff_windows
        slot->vp    = wh->vp;   // last segment vp, pre-computed in cache_diff_windows
        slot->valid = false;

        bool reuse_geo = set_stable && s_retained_skip && !wh->changed && prev->valid;

        if ( reuse_geo )
        {
            /* Geometry is in place at prev->vert_base.  Advance the write head by the padded
               alloc so the next window's prev->vert_base aligns with s_tess.vert_count. */
            slot->vert_base  = prev->vert_base;
            slot->vert_count = prev->vert_count;
            slot->vert_alloc = prev->vert_alloc;
            slot->idx_base   = prev->idx_base;
            slot->idx_count  = prev->idx_count;
            slot->idx_alloc  = prev->idx_alloc;
            s_tess.vert_count = prev->vert_base + prev->vert_alloc;
            s_tess.idx_count  = prev->idx_base  + prev->idx_alloc;

            /* Replay GPU commands from the stable cache.  lvbase is slot-local, so
               adding slot->vert_base gives the absolute vertex offset for this frame. */
            slot->cmd_base  = s_tess.cmd_count;
            slot->cmd_count = s_win_cached_count[ wi ];
            u32 nc = slot->cmd_count;
            for ( u32 k = 0; k < nc; ++k )
            {
                u32 ci = slot->cmd_base + k;
                s_tess.cmds    [ ci ]  = s_win_cached[ wi ][ k ].cmd;
                s_tess.cmd_vp  [ ci ]  = s_win_cached[ wi ][ k ].vp;
                s_tess.cmd_vbase[ ci ] = slot->vert_base + s_win_cached[ wi ][ k ].lvbase;
            }
            s_tess.cmd_count += nc;

            if ( !( s_exempt_perf_overlay && wh->win == g_gui_perf_overlay_id ) )
            {
                vert_retained += slot->vert_count;
                tri_retained  += slot->idx_count / 3u;
                ++win_retained;
            }
            slot->valid = true;
        }
        else
        {
            /* Changed, new, or unstable window: tessellate at the current write head.
               In stable mode the write head equals prev->vert_base, so this overwrites
               only the window's own prior region without disturbing its neighbours. */
            slot->vert_base       = s_tess.vert_count;
            slot->idx_base        = s_tess.idx_count;
            slot->cmd_base        = s_tess.cmd_count;
            s_tess.slot_vert_base = s_tess.vert_count;
            s_tess.force_new_cmd  = true;

            cache_tess_window( wh->win );

            slot->vert_count = s_tess.vert_count - slot->vert_base;
            slot->idx_count  = s_tess.idx_count  - slot->idx_base;
            slot->cmd_count  = s_tess.cmd_count  - slot->cmd_base;

            /* Keep the slot's padded reservation if the new geometry fits, so downstream
               slots stay put and can reuse in place next frame.  Otherwise expand and
               invalidate all downstream prev slots so they re-tessellate at their new homes. */
            bool fits = ( set_stable && prev && prev->valid
                          && slot->vert_count <= prev->vert_alloc
                          && slot->idx_count  <= prev->idx_alloc );
            if ( fits )
            {
                slot->vert_alloc = prev->vert_alloc;
                slot->idx_alloc  = prev->idx_alloc;
            }
            else
            {
                slot->vert_alloc = slot->vert_count + SLOT_VERT_PAD;
                slot->idx_alloc  = slot->idx_count  + SLOT_IDX_PAD;
                if ( set_stable )
                    for ( u32 ni = wi + 1; ni < s_slot_prev_count; ++ni )
                        s_slots_prev[ ni ].valid = false;
            }

            /* Advance the write head by the full padded reservation.  Both the reuse and
               tessellation paths use this rule, which is what keeps s_tess.vert_count ==
               prev[wi].vert_base for every window, every frame, in stable mode. */
            s_tess.vert_count = slot->vert_base + slot->vert_alloc;
            s_tess.idx_count  = slot->idx_base  + slot->idx_alloc;

            /* Write GPU commands into the stable cache for reuse next retained frame. */
            u32 nc = slot->cmd_count;
            if ( nc > WIN_SLOT_CMD_MAX ) nc = WIN_SLOT_CMD_MAX;
            s_win_cached_count[ wi ] = nc;
            for ( u32 k = 0; k < nc; ++k )
            {
                u32 ci = slot->cmd_base + k;
                s_win_cached[ wi ][ k ].cmd    = s_tess.cmds    [ ci ];
                s_win_cached[ wi ][ k ].vp     = s_tess.cmd_vp  [ ci ];
                s_win_cached[ wi ][ k ].lvbase = s_tess.cmd_vbase[ ci ] - slot->vert_base;
            }
            slot->cmd_count = nc;
            slot->valid     = true;
        }

        /* Accumulate per-slot geometry stats; exclude overlay window from totals. */
        if ( s_exempt_perf_overlay && wh->win == g_gui_perf_overlay_id )
            overlay_win = 1;
        else
        {
            total_vert += slot->vert_count;
            total_tri  += slot->idx_count / 3u;
        }

        s_dispatch[ s_dispatch_count++ ] = slot;
    }

    /* Step 3: insertion-sort dispatch pointers by z ascending (back-to-front draw order).
       Stable on equal z since insertion sort preserves relative order for equal keys. */
    for ( u32 a = 1; a < s_dispatch_count; ++a )
    {
        win_geo_slot_t* key = s_dispatch[ a ];
        u32 b = a;
        while ( b > 0 && s_dispatch[ b - 1 ]->z > key->z )
        {
            s_dispatch[ b ] = s_dispatch[ b - 1 ];
            --b;
        }
        s_dispatch[ b ] = key;
    }

    /* Publish geometry and retained stats. */
    s_stats.accum.vert_count    = total_vert;
    s_stats.accum.tri_count     = total_tri;
    s_stats.accum.win_total     = s_cache.cur_n - overlay_win;
    s_stats.accum.win_retained  = win_retained;
    s_stats.accum.vert_retained = vert_retained;
    s_stats.accum.tri_retained  = tri_retained;

    /* Track geometry high-water marks and warn once on overflow. */
    if ( s_tess.vert_count > s_tess.vert_hwm ) s_tess.vert_hwm = s_tess.vert_count;
    if ( s_tess.idx_count  > s_tess.idx_hwm  ) s_tess.idx_hwm  = s_tess.idx_count;

    if ( s_tess.overflow && !s_tess.overflow_ever )
        printf( "[gui] WARNING: draw list overflow -- geometry dropped this frame "
                "(verts capped at %u, idx capped at %u). Raise GUI_MAX_VERTS / GUI_MAX_IDX.\n",
                GUI_MAX_VERTS, GUI_MAX_IDX );
    if ( s_tess.overflow )
        s_tess.overflow_ever = true;

    static u32 prev_verts = ~0u, prev_idx = ~0u;
    if ( s_dbg_geometry && ( s_tess.vert_count != prev_verts || s_tess.idx_count != prev_idx ) )
    {
        printf( "[gui] geometry: verts %u/%u (peak %u)  idx %u/%u (peak %u)\n",
                s_tess.vert_count, GUI_MAX_VERTS, s_tess.vert_hwm,
                s_tess.idx_count,  GUI_MAX_IDX,   s_tess.idx_hwm );
        prev_verts = s_tess.vert_count;
        prev_idx   = s_tess.idx_count;
    }

    static u32 prev_win_ret = ~0u, prev_vert_ret = ~0u;
    if ( s_dbg_retained &&
         ( s_stats.accum.win_retained  != prev_win_ret ||
           s_stats.accum.vert_retained != prev_vert_ret ) )
    {
        printf( "[gui] retained: wins %u/%u  verts %u/%u  tris %u/%u\n",
                s_stats.accum.win_retained,  s_stats.accum.win_total,
                s_stats.accum.vert_retained, s_stats.accum.vert_count,
                s_stats.accum.tri_retained,  s_stats.accum.tri_count );
        prev_win_ret  = s_stats.accum.win_retained;
        prev_vert_ret = s_stats.accum.vert_retained;
    }
}

// clang-format on
/*============================================================================================*/
