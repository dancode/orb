/*==============================================================================================

    runtime_service/gui/gui_render_cache.c -- Retained frame-geometry cache (BUILD phase).

    The render pipeline has three phases.  This file is the middle one:

        EMIT   (gui_draw.c)        widgets push semantic shapes -> s_draw command list,
                                     cut into per-(win,z,vp) segments, one hash baked per command.
        BUILD  (this file)           once per frame: diff each window's commands against last
                                     frame, reuse the geometry of unchanged windows in place and
                                     tessellate only the changed ones, then z-sort the result into
                                     a dispatch table.  Output lands in s_tess (geometry) + the
                                     slot/dispatch tables here.
        SUBMIT (gui_render.c)      once per surface: upload the slots this surface owns and emit
                                     one indexed draw call per cached GPU command.

    The BUILD runs lazily on the first surface flush of the frame (cache_build_frame, guarded by
    s_frame_built) because the semantic list is shared across every surface -- the geometry it
    produces is identical no matter which surface triggers it.  gui_render_frame_reset clears the
    guard at frame_begin.

    Included by gui_backend.c after gui_render_tess.c (s_tess, tess_reset, tess_dispatch) and
    gui_draw.c (s_draw, fnv1a) -- both are read here -- and before gui_render.c, whose flush
    consumes the dispatch table this file builds.

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   // gui_id_t, gui_rect_t, GUI_MAX_* capacities
// clang-format off

/*----------------------------------------------------------------------------------------------
    Once-per-frame guard.

    cache_build_frame does the shared BUILD work on the first surface flush and stamps s_frame_built;
    later surfaces that frame see it set and reuse the slot + dispatch tables untouched.
----------------------------------------------------------------------------------------------*/

static bool s_frame_built;   // slot table + tessellation already computed this frame

// gui_render_frame_reset -- clear the guard (frame_begin, right after draw_reset, before emit).
void
gui_render_frame_reset( void )
{
    s_frame_built = false;
}

/*----------------------------------------------------------------------------------------------
    Per-frame render stats.

    accum is built up during the BUILD (geometry/retained counts) and the SUBMIT (draw_calls, summed
    across surfaces).  published is last frame's completed totals, promoted from accum at frame_begin
    so the perf overlay reads a stable snapshot one frame behind the geometry it describes.
----------------------------------------------------------------------------------------------*/

static struct
{
    gui_render_stats_t accum;          // built across this frame's BUILD + flush(es)
    gui_render_stats_t published;      // last frame's completed totals (what the overlay reads)
    u32                  draw_call_hwm;  // peak indexed draws in any single frame (lifetime)

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

// Peak draw-call count, read by the shutdown report in gui_render.c.
static u32
cache_draw_call_hwm( void )
{
    return s_stats.draw_call_hwm;
}

// Fold one surface's draw-call count into this frame's accumulator + the lifetime peak (called by flush).
static void
cache_count_draw_calls( u32 draw_calls )
{
    s_stats.accum.draw_calls += draw_calls;
    if ( draw_calls > s_stats.draw_call_hwm )
        s_stats.draw_call_hwm = draw_calls;
}

// Fold one surface's upload batch/byte count into this frame's accumulator (called by flush).
static void
cache_count_upload( u32 batches, u32 bytes )
{
    s_stats.accum.upload_batches += batches;
    s_stats.accum.upload_bytes   += bytes;
}

/*----------------------------------------------------------------------------------------------
    Retained-skip toggle.

    When on (default) a window whose per-command hash matches last frame keeps its geometry in place
    instead of re-tessellating.  Turn off to benchmark or to confirm the from-scratch path is
    correct.  Toggled via gui()->set_retained_skip (key C in sb_vulkan).
----------------------------------------------------------------------------------------------*/

static bool s_retained_skip = true;

void gui_render_set_retained_skip( bool on ) { s_retained_skip = on; }
bool gui_render_retained_skip   ( void )     { return s_retained_skip; }

/*----------------------------------------------------------------------------------------------
    Build-phase debug toggles (flip in the debugger / at startup).

    Each prints a per-frame line, only when its number changes, so a steady UI does not spam.  The
    same retained/geometry numbers are also live through gui()->render_stats() in the perf overlay.
----------------------------------------------------------------------------------------------*/

static bool s_dbg_cache    = false;  // per-window cache diff: how many windows unchanged vs total
static bool s_dbg_geometry = false;  // emitted vertex / index counts this frame (render density)
static bool s_dbg_retained = false;  // windows + verts/tris reused from last frame vs total
static bool s_exempt_perf_overlay = true;  // exclude the perf overlay's metrics from the totals

/*==============================================================================================
    Window geometry slots -- the retained store.

    Each window owns one slot caching its tessellated geometry (a vertex span, an index span, and the
    GPU draw commands that replay them).  An unchanged window copies its commands forward from last
    frame instead of re-tessellating; a changed window re-tessellates into its slot.  Slots are
    z-sorted into the dispatch table that SUBMIT walks.

    The slot tables ping-pong between two backing stores so this frame can read last frame's slots
    (s_slots_prev) while writing this frame's (s_slots) -- cache_build_frame swaps the pointers.
==============================================================================================*/

#define RENDER_MAX_WIN    32    // distinct windows tracked per frame (slots + cache diff)
#define WIN_SLOT_CMD_MAX  16    // max GPU draw cmds cached per slot; most windows have 2-4
#define SLOT_VERT_PAD     64u   // per-slot vertex slack: absorbs minor geometry growth in-place
#define SLOT_IDX_PAD      128u  // per-slot index slack (roughly 2x vertex count for quads)

/* One cached GPU draw command, packed AOS so replaying a slot touches one contiguous region.
   z is per-slot (derived from the window's max segment z, stored in win_geo_slot_t), not per
   GPU command; it was removed from this struct along with cmd_z[] in the s_tess batch header. */
typedef struct
{
    gui_gpu_cmd_t   cmd;     /* clip rect, texture slot, element count */
    u32             vp;      /* viewport this command targets          */
    u32             lvbase;  /* slot-local (0-relative) vertex base    */

} win_slot_cmd_t;

// One window's cached geometry: where it sits in the shared VB/IB and the commands that replay it.
typedef struct
{
    gui_id_t        win;
    u32             z, vp;
    u32             vert_base,  vert_count,  vert_alloc;  // VB position, actual count, padded reservation
    u32             idx_base,   idx_count,   idx_alloc;   // IB position, actual count, padded reservation
    u32             cmd_base,   cmd_count;                // range into s_tess.cmds[] owned by this window
    win_slot_cmd_t  cached[ WIN_SLOT_CMD_MAX ];           // GPU cmds; filled at tess, replayed on reuse
    bool            valid;                                // geometry tessellated at least once

} win_geo_slot_t;

static u32             s_slot_count, s_slot_prev_count;
static win_geo_slot_t  s_slots_a  [ RENDER_MAX_WIN ];     // ping-pong backing store A
static win_geo_slot_t  s_slots_b  [ RENDER_MAX_WIN ];     // ping-pong backing store B
static win_geo_slot_t* s_slots      = s_slots_a;          // current frame's slots
static win_geo_slot_t* s_slots_prev = s_slots_b;          // previous frame's slots
static win_geo_slot_t* s_dispatch [ RENDER_MAX_WIN ];     // z-sorted pointers into s_slots
static u32             s_dispatch_count;

/* No separate prev-geometry buffers: s_tess.verts/indices are static arrays that persist between
   frames, so each window's geometry remains at its prev->vert_base position until overwritten.  The
   reuse path exploits this by advancing the write head by vert_alloc (not vert_count), leaving a
   SLOT_VERT_PAD gap that absorbs minor in-place growth without touching adjacent slots. */

/*==============================================================================================
    Change detection (BUILD step 1) -- diff each window's commands against last frame.

    Each frame folds every window's per-command hashes (baked at emit, draw_hash_cmd) into one
    per-window hash and compares against last frame's table (double-buffered cur/prev).  A window
    whose hash matches is unchanged and may reuse its geometry; any_changed is the coarse signal
    that some window appeared, vanished, or changed.

    The per-command hash is deliberately deep for the two command types that point into side pools
    (TEXT -> text_pool, POLYLINE -> points): a same-length, same-offset edit ("3" -> "4") leaves the
    command struct byte-identical, so the pooled bytes are folded in too (see draw_hash_cmd).
==============================================================================================*/

// Per-window diff record.  cur[] is rebuilt every frame; prev[] is last frame's snapshot.  z/vp are
// accumulated here (max-z, last-vp across the window's segments) so the slot build needs no rescan.
typedef struct
{
    gui_id_t win;
    u32        hash;
    u32        z;        // max segment z this frame (slot dispatch order)
    u32        vp;       // viewport of the last segment this frame
    bool       changed;  // hash mismatched or window is new

} render_win_hash_t;

static struct
{
    render_win_hash_t cur  [ RENDER_MAX_WIN ];  // this frame's per-window records
    render_win_hash_t prev [ RENDER_MAX_WIN ];  // last frame's hashes, for the diff

    u32  cur_n, prev_n;                         // valid entries in cur[] / prev[]
    u32  unchanged;                             // windows whose hash matched last frame
    bool any_changed;                           // a window appeared / vanished / changed

} s_cache = { .any_changed = true };            // start true: guarantees the first frame builds

/* True when the PREVIOUS frame produced any render change.  Read by frame_begin before this frame's
   cache_build_frame runs; s_cache.any_changed still holds last frame's result then.  When false for a
   frame with no input and no animation, the host may skip the widget emit. */
bool
gui_render_any_changed( void )
{
    return s_cache.any_changed;
}

// cache_diff_windows -- fold per-command hashes into per-window hashes and diff against last frame.
// Runs BEFORE tessellation so a fully unchanged frame can skip tess entirely (the retained-skip path).
static void
cache_diff_windows( void )
{
    const gui_cmd_seg_t* segs = s_draw.segs;
    u32                  nseg = s_draw.seg_count;

    /* Roll each segment's pre-baked command hashes into its window's accumulated hash, also tracking
       max-z (dispatch order) and last-vp (surface routing) so the slot loop needs no second scan.
       Segments of one window arrive in increasing lo (emit order), so the folds are stable. */
    s_cache.cur_n = 0;
    for ( u32 si = 0; si < nseg; ++si )
    {
        if ( segs[ si ].lo == segs[ si ].hi ) continue;   // empty span

        gui_id_t win = segs[ si ].win;
        u32        bi  = 0;
        for ( ; bi < s_cache.cur_n; ++bi )
            if ( s_cache.cur[ bi ].win == win ) break;
        if ( bi == s_cache.cur_n )
        {
            if ( s_cache.cur_n >= RENDER_MAX_WIN ) continue;   // overflow: treated as changed
            s_cache.cur[ bi ].win  = win;
            s_cache.cur[ bi ].hash = 2166136261u;
            s_cache.cur[ bi ].z    = 0;
            s_cache.cur[ bi ].vp   = 0;
            ++s_cache.cur_n;
        }

        // Fold sort key + viewport + font into the hash so a raise, surface move, or font swap
        // invalidates the window (the font is the segment's atlas context, see draw_set_font).
        u32 h = s_cache.cur[ bi ].hash;
        h = fnv1a( h, &segs[ si ].z,    sizeof segs[ si ].z );
        h = fnv1a( h, &segs[ si ].vp,   sizeof segs[ si ].vp );
        h = fnv1a( h, &segs[ si ].font, sizeof segs[ si ].font );
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
            h = fnv1a( h, &s_draw.cmd_hashes[ i ], sizeof s_draw.cmd_hashes[ i ] );
        s_cache.cur[ bi ].hash = h;

        if ( segs[ si ].z > s_cache.cur[ bi ].z )
            s_cache.cur[ bi ].z  = segs[ si ].z;
        s_cache.cur[ bi ].vp = segs[ si ].vp;
    }

    // Diff against last frame: a window is unchanged iff it existed with the same hash.
    s_cache.unchanged   = 0;
    s_cache.any_changed = ( s_cache.cur_n != s_cache.prev_n );
    for ( u32 i = 0; i < s_cache.cur_n; ++i )
    {
        bool match = false;
        for ( u32 j = 0; j < s_cache.prev_n; ++j )
            if ( s_cache.prev[ j ].win == s_cache.cur[ i ].win )
            {
                match = ( s_cache.prev[ j ].hash == s_cache.cur[ i ].hash );
                break;
            }
        s_cache.cur[ i ].changed = !match;
        if ( match ) ++s_cache.unchanged;
        else         s_cache.any_changed = true;
    }

    // Promote this frame's table to prev for next frame's diff.
    memcpy( s_cache.prev, s_cache.cur, s_cache.cur_n * sizeof( render_win_hash_t ) );
    s_cache.prev_n = s_cache.cur_n;

    if ( s_dbg_cache && s_cache.any_changed )
        printf( "[gui] cache: frame changed -- %u/%u windows unchanged\n",
                s_cache.unchanged, s_cache.cur_n );
}

/*==============================================================================================
    Per-window tessellation (BUILD step 2 helper) -- tessellate one window into its slot.

    Gathers a window's segments from s_draw.segs and builds a clip-sorted permutation into a local
    order buffer, then hands it to tess_dispatch.  Grouping by clip (first-seen order) makes
    equal-clip shapes tessellate back-to-back so they merge into one GPU batch.  z is NOT sorted
    here -- the window occupies one slot whose dispatch z is its raise z, keeping all of a window's
    geometry (chrome + body) contiguous regardless of internal z values; cache_build_frame z-sorts
    the slot table afterwards.
==============================================================================================*/

// rect_empty (a zero-area clip) is defined in gui_draw.c, included before this unit.

#define RENDER_MAX_CLIP_GROUPS  64   // distinct clip indices groupable within one window

static void
cache_tess_window( gui_id_t win )
{
    const gui_cmd_seg_t* segs = s_draw.segs;
    u32                    nseg = s_draw.seg_count;

    /* Distinct clip INDICES for this window's commands, in first-seen order.  Comparing u8 indices
       is a single equality test vs. the old four-float clip_eq; the actual rect lives in
       s_draw.clip_table and is resolved at tessellation time (tess_dispatch). */
    u8   clips[ RENDER_MAX_CLIP_GROUPS ];
    u32  nc       = 0;
    bool overflow = false;

    for ( u32 si = 0; si < nseg && !overflow; ++si )
    {
        if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            u8 ci = s_draw.cmds[ i ].clip_idx;
            if ( rect_empty( s_draw.clip_table[ ci ] ) ) continue;
            bool seen = false;
            for ( u32 g = 0; g < nc; ++g )
                if ( clips[ g ] == ci ) { seen = true; break; }
            if ( !seen )
            {
                if ( nc >= RENDER_MAX_CLIP_GROUPS ) { overflow = true; break; }
                clips[ nc++ ] = ci;
            }
        }
    }

    /* Clip-sorted permutation, plus a parallel font id per ordered entry.  The permutation regroups
       commands by clip ACROSS segments, so a command's font cannot be inferred from its position at
       dispatch -- but here the build still knows the owning segment (si), so we record segs[si].font
       alongside each index.  The font thus travels with its commands through the reorder while
       staying a per-segment property (no per-command field).  tess_dispatch re-activates it per run.
       Static since cache_build_frame is guarded (single call per frame, single-threaded). */
    static u32 win_order[ GUI_MAX_CMDS ];
    static u32 win_font [ GUI_MAX_CMDS ];
    u32 n = 0;

    if ( overflow )
    {
        /* Too many distinct clips: fall back to natural emit order (correct, just less merged). */
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
        for ( u32 g = 0; g < nc; ++g )
            for ( u32 si = 0; si < nseg; ++si )
            {
                if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
                for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                    if ( s_draw.cmds[ i ].clip_idx == clips[ g ] )
                        { win_font[ n ] = segs[ si ].font; win_order[ n++ ] = i; }
            }
    }

    tess_dispatch( s_draw.cmds, win_order, win_font, n );
}

/*==============================================================================================
    cache_build_frame (BUILD step 2) -- diff, reuse or re-tessellate per window, z-sort.

    Runs once per frame (guarded by s_frame_built): the first surface flush triggers it; every other
    surface that frame reuses s_tess + s_dispatch.  Produces the geometry (s_tess.verts/indices), the
    per-window slot table, and the back-to-front dispatch order -- all surface-independent.
==============================================================================================*/

static void
cache_build_frame( void )
{
    if ( s_frame_built )
        return;
    s_frame_built = true;

    // Close the still-open final segment so the diff and per-window tess see its full [lo,hi).
    if ( s_draw.seg_count > 0 )
        s_draw.segs[ s_draw.seg_count - 1 ].hi = s_draw.cmd_count;

    // Step 1: diff each window's commands against last frame (fills s_cache + any_changed).
    cache_diff_windows();

    // Rotate slot tables: last frame's s_slots becomes s_slots_prev; build fresh into s_slots.
    win_geo_slot_t* tmp = s_slots_prev;
    s_slots_prev      = s_slots;
    s_slots           = tmp;
    s_slot_prev_count = s_slot_count;
    s_slot_count      = 0;
    s_dispatch_count  = 0;

    tess_reset();

    /* In-place geometry reuse: when the window set is identical to last frame (same windows, same
       emit order, all slots valid), unchanged geometry is already sitting in s_tess at its
       prev->vert_base position.  The write head advances by vert_alloc (not vert_count) so the
       padding gap is preserved; this keeps the next window's prev->vert_base equal to
       s_tess.vert_count at the start of its iteration, holding every window in place without any
       memmove.  Changed windows tessellate at that same write head (effectively in place over their
       own old data) and stay within their alloc.

       When the set is NOT stable (window added/removed/reordered), every window tessellates
       sequentially and gets a fresh alloc; structural changes are rare so the cost is fine.

       Overflow: if a changed window emits more verts than its prev->vert_alloc it has written into
       the next slot's territory.  We grow its alloc and invalidate all downstream prev entries so
       they re-tessellate fresh this frame instead of reading corrupted data.  With SLOT_VERT_PAD=64
       this only triggers when a window gains more than ~16 rects at once. */
    bool set_stable = ( s_slot_prev_count == s_cache.cur_n );
    for ( u32 wi = 0; set_stable && wi < s_cache.cur_n; ++wi )
        if ( s_slots_prev[ wi ].win != s_cache.cur[ wi ].win || !s_slots_prev[ wi ].valid )
            set_stable = false;

    u32 vert_retained = 0, tri_retained = 0, win_retained = 0;

    for ( u32 wi = 0; wi < s_cache.cur_n; ++wi )
    {
        const render_win_hash_t* wh   = &s_cache.cur[ wi ];
        win_geo_slot_t*          slot = &s_slots[ s_slot_count++ ];

        slot->win   = wh->win;
        slot->z     = wh->z;    // pre-computed during diff: max segment z
        slot->vp    = wh->vp;   // pre-computed during diff: last segment vp
        slot->valid = false;

        // In stable mode prev[wi] is guaranteed to match (verified above).
        win_geo_slot_t* prev = ( set_stable && wi < s_slot_prev_count ) ? &s_slots_prev[ wi ] : NULL;

        bool reuse_geo = set_stable && s_retained_skip && !wh->changed && prev && prev->valid;

        if ( reuse_geo )
        {
            /* Unchanged window: geometry is in place at prev->vert_base.  Advance the write head past
               the padded alloc so the next window finds its own prev->vert_base at exactly
               s_tess.vert_count -- no memmove required. */
            slot->vert_base  = prev->vert_base;
            slot->vert_count = prev->vert_count;
            slot->vert_alloc = prev->vert_alloc;
            slot->idx_base   = prev->idx_base;
            slot->idx_count  = prev->idx_count;
            slot->idx_alloc  = prev->idx_alloc;

            s_tess.vert_count = prev->vert_base + prev->vert_alloc;
            s_tess.idx_count  = prev->idx_base  + prev->idx_alloc;

            // Replay cached GPU draw commands; vert_base is unchanged so lvbase needs no fixup.
            slot->cmd_base  = s_tess.cmd_count;
            slot->cmd_count = prev->cmd_count;
            u32 nc = prev->cmd_count;
            for ( u32 k = 0; k < nc; ++k )
            {
                u32 ci = slot->cmd_base + k;
                s_tess.cmds    [ ci ]  = prev->cached[ k ].cmd;
                s_tess.cmd_vp  [ ci ]  = prev->cached[ k ].vp;
                s_tess.cmd_vbase[ ci ] = slot->vert_base + prev->cached[ k ].lvbase;
            }
            s_tess.cmd_count += nc;
            memcpy( slot->cached, prev->cached, nc * sizeof( win_slot_cmd_t ) );

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
            /* Changed, new, or unstable window: tessellate at the current write head.  In stable mode
               the head is kept equal to prev->vert_base by the uniform "advance by alloc" rule (here
               and in the reuse branch), so this tessellates in place over the window's own previous
               geometry without disturbing its neighbours. */
            slot->vert_base        = s_tess.vert_count;
            slot->idx_base         = s_tess.idx_count;
            slot->cmd_base         = s_tess.cmd_count;
            s_tess.slot_vert_base  = s_tess.vert_count;
            s_tess.force_new_cmd   = true;

            cache_tess_window( wh->win );

            slot->vert_count = s_tess.vert_count - slot->vert_base;
            slot->idx_count  = s_tess.idx_count  - slot->idx_base;
            slot->cmd_count  = s_tess.cmd_count  - slot->cmd_base;

            /* Pick this slot's padded reservation.  If the new geometry fits inside the window's
               previous reservation, keep it -- downstream slots stay put and keep reusing in place.
               Otherwise grow the reservation (which shifts every later slot down) and invalidate the
               downstream prev slots so they re-tessellate at their new homes rather than read stale
               in-place geometry this grown window just overwrote. */
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

            /* Reserve the full padded span so the next slot's write head lands exactly on its own
               prev->vert_base.  This single rule -- shared with the reuse branch -- is what keeps
               s_tess.vert_count == prev[wi].vert_base for every window, in every frame. */
            s_tess.vert_count = slot->vert_base + slot->vert_alloc;
            s_tess.idx_count  = slot->idx_base  + slot->idx_alloc;

            // Cache GPU draw commands for potential reuse next frame.
            u32 nc = slot->cmd_count;
            if ( nc > WIN_SLOT_CMD_MAX ) nc = WIN_SLOT_CMD_MAX;
            for ( u32 k = 0; k < nc; ++k )
            {
                u32 ci = slot->cmd_base + k;
                slot->cached[ k ].cmd    = s_tess.cmds    [ ci ];
                slot->cached[ k ].vp     = s_tess.cmd_vp  [ ci ];
                slot->cached[ k ].lvbase = s_tess.cmd_vbase[ ci ] - slot->vert_base;
            }
            slot->cmd_count = nc;
            slot->valid     = true;
        }

        s_dispatch[ s_dispatch_count++ ] = slot;
    }

    // Insertion sort dispatch pointers by slot->z ascending so slots draw back-to-front.
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

    // Aggregate exact geometry and frontend commands, skipping the perf_overlay if exempted.
    u32 total_vert = 0, total_tri = 0, total_cmd = 0, overlay_win = 0;
    
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        if ( s_exempt_perf_overlay && s_slots[ i ].win == g_gui_perf_overlay_id )
        {
            overlay_win = 1;
            continue;
        }
        total_vert += s_slots[ i ].vert_count;
        total_tri  += s_slots[ i ].idx_count / 3u;
    }

    for ( u32 i = 0; i < s_draw.seg_count; ++i )
    {
        if ( s_exempt_perf_overlay && s_draw.segs[ i ].win == g_gui_perf_overlay_id )
            continue;
        total_cmd += s_draw.segs[ i ].hi - s_draw.segs[ i ].lo;
    }

    // Geometry stats.
    s_stats.accum.cmd_count  = total_cmd;
    s_stats.accum.vert_count = total_vert;
    s_stats.accum.tri_count  = total_tri;
    
    // Retained stats -- published via gui_render_stats().
    s_stats.accum.win_total     = s_cache.cur_n - overlay_win;
    s_stats.accum.win_retained  = win_retained;
    s_stats.accum.vert_retained = vert_retained;
    s_stats.accum.tri_retained  = tri_retained;

    // Track peak tessellated geometry.  Warn once on the first overflow.
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
        printf( "[gui] geometry this frame: verts %u/%u (peak %u), idx %u/%u (peak %u)\n",
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
