/*==============================================================================================

    runtime_service/gui/backend/pipeline/gui_build_cache.c -- Retained frame-geometry cache (BUILD phase).

    The render pipeline has three phases.  This file is the middle one:

        EMIT   (gui_emit_draw.c)  widgets push semantic shapes -> s_draw command list,
                                   cut into per-(win,z,vp,font,band) segments, one hash baked per command.
        BUILD  (this file)        once per frame: diff each window's commands against last frame,
                                   reuse unchanged geometry in place, tessellate changed windows,
                                   then z-sort the result into a dispatch table.
        RENDER (gui_render.c)     once per surface: upload changed geometry and emit one indexed
                                   draw call per cached GPU command.

    BUILD runs lazily on the first surface flush (cache_build_frame, guarded by s_frame_built)
    because the semantic command list is shared across every surface -- the geometry it produces
    is surface-independent.  gui_build_frame_reset clears the guard at frame_begin.

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"
// clang-format off

/* Reverse id->name lookup, defined later in this unit (gui_debug_overlay.c) -- used by the overflow
   report below to name the window that blew the geometry caps.  Returns the registered title in
   debug builds (windows register via DBG_NAME in window_begin_ex), NULL when the name registry is
   compiled out (release) or the id was never registered. */
const char* gui_debug_name( gui_id_t id );

/*----------------------------------------------------------------------------------------------
    Once-per-frame guard.

    The first surface flush triggers cache_build_frame and stamps s_frame_built; later surfaces
    reuse the slot and dispatch tables untouched.
----------------------------------------------------------------------------------------------*/

static bool s_frame_built;

void
gui_build_frame_reset( void )
{
    s_frame_built = false;
}

/*----------------------------------------------------------------------------------------------
    Per-frame render stats.

    accum is built by two phases that do NOT run on the same schedule: BUILD (cache_diff_windows /
    cache_build_frame -- cmd_count, vert_count, tri_count, win_total, win_retained, vert_retained,
    tri_retained) runs at most once per REAL frame, guarded by s_frame_built, and is skipped
    entirely on an idle frame (frame_dirty()==false); SUBMIT (cache_count_draw_calls /
    cache_count_upload -- draw_calls, upload_batches, upload_bytes) runs every single frame, real
    or idle, once per surface flush, because the GPU replays cached geometry every frame regardless.

    gui_build_stats_publish runs at every frame_begin, real or idle.  It must NOT blanket-zero
    accum: the BUILD fields are plain assignments ("="), not accumulations, so on an idle frame
    they still hold the last real frame's correct totals and should be left alone -- publishing
    them again is exactly right (nothing changed).  Only the SUBMIT fields need a per-frame reset,
    since they are "+=" summed across this frame's surface flushes and would otherwise double-count
    forever.  Zeroing the whole struct here (the previous bug) made win_total/win_retained/etc
    collapse to 0 after any idle frame, which read on screen as the retained-window count randomly
    flickering between correct and zero every time the idle-skip kicked in -- nothing was actually
    wrong with retention, only with how the overlay reported it.
----------------------------------------------------------------------------------------------*/

static struct
{
    gui_render_stats_t accum;        // BUILD fields persist across idle frames; SUBMIT fields reset every frame
    gui_render_stats_t published;    // last frame's completed totals (what the overlay reads)
    u32                draw_call_hwm; // peak indexed draws in any single frame (lifetime)

} s_stats;

gui_render_stats_t
gui_render_stats( void )
{
    return s_stats.published;
}

void
gui_build_stats_publish( void )
{
    s_stats.published = s_stats.accum;
    s_stats.accum.draw_calls       = 0;
    s_stats.accum.upload_batches   = 0;
    s_stats.accum.upload_bytes     = 0;
    s_stats.accum.volatile_patched = 0;   // per-frame event count, same reset rule as draw_calls
}

/* Defined here (forward-declared in gui_build_volatile.c, included just above this file) because
   it needs s_stats.  Counts a volatile row patched in place this frame, whether via idle replay
   (gui_update_volatile) or a live real-frame reuse-patch (volatile_patch_reused_window). */
static void
cache_count_volatile_patch( u32 n )
{
    s_stats.accum.volatile_patched += n;
}

// Peak draw-call count, read by the shutdown report in gui_render.c.
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

    When enabled (default: s_caps.retained_cache) a window whose per-command hash matches last
    frame keeps its geometry in place instead of re-tessellating.  Disable to benchmark or verify
    the from-scratch path.  Toggled via gui()->set_retained_skip (key C in sb_vulkan), which flips
    the same s_caps field latched at gui_backend_init -- there is only the one flag.
----------------------------------------------------------------------------------------------*/

void gui_build_set_retained_skip( bool on ) { s_caps.retained_cache = on; }
bool gui_build_retained_skip    ( void )    { return s_caps.retained_cache; }

/*----------------------------------------------------------------------------------------------
    Build-phase debug toggles.

    s_caps.stats_trace (set at gui_backend_init, default off) gates all three prints below; each
    prints a per-frame line only when its value changes, so a steady UI does not spam.  The same
    numbers are live through gui()->render_stats() in the perf overlay regardless of the flag.
----------------------------------------------------------------------------------------------*/

/* Debug-band windows (GUI_WIN_DEBUG_BAND: the perf overlay, the pipeline dashboard, their
   popups/tooltips) are exempted from: (1) the vert/tri/win totals they may themselves display
   (gui_render_stats_t), and (2) contributing their ever-changing hashes to any_changed / the
   frame_dirty signal that drives idle-skip.  Without (2), simply having a live readout visible
   would keep the whole app rebuilding every frame regardless of anything else being idle.
   Debug-band windows still hash, diff, and retessellate normally -- they render; only the
   metrics ignore them, and the slot packer places them after every main-band slot (see the
   band-major sort in cache_diff_windows). */

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

/* RENDER_MAX_WIN / SLOT_VERT_PAD / SLOT_IDX_PAD live in gui_backend.h (the dashboard snapshot
   types are sized by them). */
#define WIN_SLOT_CMD_MAX  24    // max GPU draw commands cached per slot; most windows have 2-4,
                                // a volatile block adds its own commands + reserved dormant pads

/* One cached GPU draw command.  Packed AOS so replaying a slot's commands touches one region.
   z is per-slot (the window's max segment z), not per-command; lvbase/libase are slot-local so
   the reuse path needs no fixup when the slot's absolute vert_base/idx_base is unchanged. */
typedef struct
{
    gui_gpu_cmd_t cmd;     // clip rect, texture slot, element count
    u32           vp;      // viewport this command targets
    u32           lvbase;  // vertex base relative to slot->vert_base (0-relative)
    u32           libase;  // index base relative to slot->idx_base (the cmd's first_index seed)

} win_slot_cmd_t;

/* One window's cached geometry position and the command range that replays it. */
typedef struct
{
    gui_id_t win;
    u32      z, vp;
    u32      band;                               // arena band (0 = main UI, 1 = debug/diagnostic)
    u32      vert_base, vert_count, vert_alloc;  // VB: absolute position, actual count, padded reservation
    u32      idx_base,  idx_count,  idx_alloc;   // IB: absolute position, actual count, padded reservation
    u32      cmd_base,  cmd_count;               // range into s_tess.cmds[] for this window
    u32      tess_gen;                           // generation of the tess pass that produced the geometry
    bool     valid;                              // true once geometry has been tessellated at least once

} win_geo_slot_t;

/* Stable GPU command cache -- outside the ping-pong arrays so the reuse path never copies it.
   Indexed by slot position wi, valid in set_stable mode (same window set and order as prev frame).
   Written when a window tessellates; read every retained frame until the window changes again. */
static win_slot_cmd_t   s_win_cached      [ RENDER_MAX_WIN ][ WIN_SLOT_CMD_MAX ];
static u32              s_win_cached_count[ RENDER_MAX_WIN ];

static u32              s_slot_count, s_slot_prev_count;
static win_geo_slot_t   s_slots_a  [ RENDER_MAX_WIN ];
static win_geo_slot_t   s_slots_b  [ RENDER_MAX_WIN ];
static win_geo_slot_t*  s_slots      = s_slots_a;   // current frame (write)
static win_geo_slot_t*  s_slots_prev = s_slots_b;   // previous frame (read)
static win_geo_slot_t*  s_dispatch [ RENDER_MAX_WIN ];
static u32              s_dispatch_count;

/* Volatile widgets (gui_volatile_cb_open/_stamp/_close, gui_update_volatile, the registry and
   volatile_patch) live in their own file -- backend/pipeline/gui_build_volatile.c, included right
   before this one; see that file's header for the full feature description.  The pieces that stay HERE
   are the three helpers it forward-declares (cache_count_volatile_patch above,
   cache_slot_lookup / cache_invalidate_window below) because they touch s_slots / s_cache /
   s_stats, plus the per-window tessellation generation (s_tess_gen_next) that anchors the
   patch-time staleness check. */

/* Monotonic tessellation-pass counter.  Every window retess stamps its slot with a fresh value;
   a volatile row captured during that pass records the same value, and a patch is only legal
   while they still match -- the row's slot-relative offsets provably describe the slot's current
   contents.  Reuse frames carry the generation forward untouched. */
static u32 s_tess_gen_next;

/* Resolve a window's CURRENT slot position + generation by id (forward-declared in
   gui_build_volatile.c).  Reads whatever s_slots currently holds: during cache_diff_windows and
   on idle frames that is the last completed frame's table; during cache_build_frame's slot loop
   it already contains this frame's slots built so far -- both exactly what a caller wants. */
static bool
cache_slot_lookup( gui_id_t win, u32* vert_base, u32* idx_base, u32* cmd_base, u32* tess_gen )
{
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        if ( s_slots[ i ].win != win || !s_slots[ i ].valid )
            continue;
        *vert_base = s_slots[ i ].vert_base;
        *idx_base  = s_slots[ i ].idx_base;
        *cmd_base  = s_slots[ i ].cmd_base;
        *tess_gen  = s_slots[ i ].tess_gen;
        return true;
    }
    return false;
}

/* Far edge of all LIVE geometry -- the floor past which s_tess is free scratch space.  Forward-
   declared in gui_build_volatile.c: a volatile patch tessellates into scratch at s_tess.vert_count
   and must assert it is at/beyond this, or the scratch scribbles through a live slot's geometry
   (the tooltip-vs-pulse collision, fixed 2026-07-04).

   Spans BOTH slot tables: mid-build-loop, s_slots holds only the windows placed so far, while a
   reused window not yet reprocessed still has its geometry sitting at LAST frame's position
   (s_slots_prev) -- exactly where the buggy inline patch scribbled over the tooltip.  Taking the
   max over both makes the guard see that still-live geometry the current table hasn't caught up to
   yet.  On idle frames both tables are last frame's, so the max is simply the true tail. */
static void
cache_slots_extent( u32* out_vert_end, u32* out_idx_end )
{
    u32 ve = 0, ie = 0;
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        if ( !s_slots[ i ].valid )
            continue;
        u32 v = s_slots[ i ].vert_base + s_slots[ i ].vert_alloc;
        u32 x = s_slots[ i ].idx_base  + s_slots[ i ].idx_alloc;
        if ( v > ve ) ve = v;
        if ( x > ie ) ie = x;
    }
    for ( u32 i = 0; i < s_slot_prev_count; ++i )
    {
        if ( !s_slots_prev[ i ].valid )
            continue;
        u32 v = s_slots_prev[ i ].vert_base + s_slots_prev[ i ].vert_alloc;
        u32 x = s_slots_prev[ i ].idx_base  + s_slots_prev[ i ].idx_alloc;
        if ( v > ve ) ve = v;
        if ( x > ie ) ie = x;
    }
    *out_vert_end = ve;
    *out_idx_end  = ie;
}

/* cache_invalidate_window lives below, after s_cache is declared. */

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
    u32      z;              // max segment z this frame (governs slot dispatch order)
    u32      vp;             // viewport of the last segment this frame
    u32      band;           // arena band: sticky OR across segments (any debug seg = debug window)
    bool     changed;        // hash mismatched, window is new, or force_changed this frame
    bool     force_changed;  // a volatile row in this window needs a (re)capture -- tessellate
                             // regardless of the hash (which excludes volatile commands entirely)

} render_win_hash_t;

/* Band-major, win-minor record ordering.  Sorting debug-band records after every main-band
   record is what packs their slots at the TAIL of the vertex/index arena: the placement loop
   walks records in this order with one bottom-up write head, so main-band layout is byte-
   identical to a world where the debug UI does not exist, and the debug UI's per-frame churn
   can only downstream-invalidate other debug slots.  Both cur[] and prev[] sort with this same
   rule every frame, preserving the sorted-alignment invariant set_stable relies on. */
static inline bool
cache_rec_before( const render_win_hash_t* a, const render_win_hash_t* b )
{
    if ( a->band != b->band )
        return a->band < b->band;
    return a->win < b->win;
}

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
gui_build_any_changed( void )
{
    return s_cache.any_changed;
}

/* Force `win` to re-tessellate next frame (forward-declared in gui_build_volatile.c): corrupt its
   stored hash so the diff mismatches, and raise any_changed so the host sees a dirty frame.  The
   recovery path for a failed volatile patch -- the retess recaptures the row at its recorded
   larger reservation. */
static void
cache_invalidate_window( gui_id_t win )
{
    for ( u32 i = 0; i < s_cache.prev_n; ++i )
        if ( s_cache.prev[ i ].win == win )
        {
            s_cache.prev[ i ].hash ^= 0xA5A5A5A5u;
            break;
        }
    s_cache.any_changed = true;
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
    u8  clip_used[ GUI_MAX_CLIP_RECTS ] = { 0 };   /* clip table entries a band-0 command references */
    for ( u32 si = 0; si < nseg; ++si )
    {
        if ( segs[ si ].lo == segs[ si ].hi ) continue;   // empty span

        gui_id_t win = segs[ si ].win;

        if ( segs[ si ].band == 0 )   /* debug-band UI never counts in the stats it displays */
            total_cmd += segs[ si ].hi - segs[ si ].lo;

        /* Find or create the per-window record. */
        u32 bi = 0;
        for ( ; bi < s_cache.cur_n; ++bi )
            if ( s_cache.cur[ bi ].win == win ) break;
        if ( bi == s_cache.cur_n )
        {
            /* Overflow: a window past RENDER_MAX_WIN gets no record, no slot, and never
               tessellates -- it is DROPPED from rendering this frame, not just uncached.
               Warn once (log + break-once assert, same treatment as the draw-list overflow
               below) so a too-small cap is not silently invisible windows.  Non-fatal: skip
               past the assert (or a release build) and the surviving windows still render. */
            if ( s_cache.cur_n >= RENDER_MAX_WIN )
            {
                static bool warned = false;
                if ( !warned )
                {
                    printf( "[gui] WARNING: more than %u windows this frame -- extra windows "
                            "are not rendered. Raise RENDER_MAX_WIN.\n", RENDER_MAX_WIN );
                    fflush( stdout );   /* flush the diagnostic before the once-assert can trap */
                    warned = true;
                }
                ORB_ASSERT_MSG_ONCE( false, "gui window overflow -- more than RENDER_MAX_WIN "
                                            "windows; extra windows dropped. Raise RENDER_MAX_WIN "
                                            "(gui_backend.h)" );
                continue;
            }
            s_cache.cur[ bi ] = ( render_win_hash_t ){ win, 2166136261u, 0, 0, 0, false, false };
            ++s_cache.cur_n;
        }

        /* Fold z, vp, font, and the shared-atlas generation into the hash.  The font id alone is not
           enough: a live re-bake (font_load_into) changes glyph geometry, and a shared-atlas repack
           can shift every tenant's UVs, all while the font id and the (now shared, stable) bindless
           index are unchanged.  res_atlas_generation bumps on any such structural change, so folding
           it forces the affected windows to re-tessellate against the new packing. */
        u32 gen = res_atlas_generation();
        u32 h   = s_cache.cur[ bi ].hash;
        h = fnv1a_u32( h, segs[ si ].z    );
        h = fnv1a_u32( h, segs[ si ].vp   );
        h = fnv1a_u32( h, segs[ si ].font );
        h = fnv1a_u32( h, segs[ si ].band );   /* band flip must re-tessellate (slot changes ends) */
        h = fnv1a_u32( h, gen              );
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            /* Clip usage is marked before the volatile skip below -- a volatile command still
               draws under its clip, it only stays out of the window hash. */
            if ( segs[ si ].band == 0 )
                clip_used[ s_draw.cmds[ i ].clip_idx ] = 1;

            /* A volatile-tagged command NEVER participates in its window's hash -- the block is
               presentation-only by contract and patched out of band (gui_update_volatile on idle
               frames, volatile_patch_reused_window on reused real frames), so its ever-drifting
               bytes must not force the whole window to retessellate, and the hash behaves
               identically whether the retained skip is on or off.  The one thing checked here is
               whether the row still has a live capture for this window's current slot; if not
               (first appearance, retired by a failed patch, or the slot rebuilt without it), the
               window is forced CHANGED so tessellation runs and (re)captures it. */
            gui_id_t vid = s_draw.cmd_volatile_id[ i ];
            if ( vid != GUI_ID_NONE )
            {
                if ( volatile_row_needs_capture( vid ) )
                    s_cache.cur[ bi ].force_changed = true;
                continue;
            }
            h = fnv1a_u32( h, s_draw.cmd_hashes[ i ] );
        }
        s_cache.cur[ bi ].hash = h;

        if ( segs[ si ].z > s_cache.cur[ bi ].z ) s_cache.cur[ bi ].z = segs[ si ].z;
        s_cache.cur[ bi ].vp = segs[ si ].vp;
        if ( segs[ si ].band != 0 ) s_cache.cur[ bi ].band = 1;   /* sticky: any debug seg tags the window */
    }

    /* Sort cur[] band-major, win-minor (cache_rec_before) -- debug-band windows pack after every
       main-band window.  Insertion sort over RENDER_MAX_WIN = 32 elements: O(n) when the window
       set is stable (the common case, already sorted from last frame), O(n^2) at worst.
       prev[] is kept in the same order via the memcpy below, so the diff is a single linear scan. */
    for ( u32 a = 1; a < s_cache.cur_n; ++a )
    {
        render_win_hash_t key = s_cache.cur[ a ];
        u32 b = a;
        while ( b > 0 && cache_rec_before( &key, &s_cache.cur[ b - 1 ] ) )
        {
            s_cache.cur[ b ] = s_cache.cur[ b - 1 ];
            --b;
        }
        s_cache.cur[ b ] = key;
    }

    /* Pass 2: diff against last frame.  Both arrays share the (band, win) sort order, so one
       linear scan suffices -- O(cur_n + prev_n) instead of the O(n^2) nested scan. */
    s_cache.unchanged   = 0;
    s_cache.any_changed = ( s_cache.cur_n != s_cache.prev_n );
    u32 pj = 0;
    for ( u32 i = 0; i < s_cache.cur_n; ++i )
    {
        while ( pj < s_cache.prev_n && cache_rec_before( &s_cache.prev[ pj ], &s_cache.cur[ i ] ) )
            ++pj;
        bool match = ( pj < s_cache.prev_n
                       && s_cache.prev[ pj ].win  == s_cache.cur[ i ].win
                       && s_cache.prev[ pj ].band == s_cache.cur[ i ].band
                       && s_cache.prev[ pj ].hash == s_cache.cur[ i ].hash );
        bool changed = !match || s_cache.cur[ i ].force_changed;
        s_cache.cur[ i ].changed = changed;
        if ( !changed ) ++s_cache.unchanged;
        else if ( s_cache.cur[ i ].band == 0 )
            s_cache.any_changed = true;

        /* A debug-band readout's own text (live FPS/ms/vert counters) changes practically every
           real frame it is visible -- if that alone kept any_changed true, gui_build_any_changed()
           would report "something changed" forever and frame_dirty would never go false again,
           silently defeating idle-skip for the WHOLE app for as long as the readout is on screen.
           s_cache.cur[i].changed above still flags true so cache_build_frame retessellates the
           readout's own slot (its digits really did change); only the GLOBAL any_changed signal
           ignores debug-band windows. */
    }

    /* Promote this frame's sorted table to prev for next frame's diff. */
    memcpy( s_cache.prev, s_cache.cur, s_cache.cur_n * sizeof( render_win_hash_t ) );
    s_cache.prev_n = s_cache.cur_n;

    s_stats.accum.cmd_count = total_cmd;

    /* Distinct clip rects the main band drew under this frame.  Counted by command reference, not
       raw table size (s_draw.clip_table_n): the raw table also holds debug-band pushes and pushes
       no surviving command references, so it overstates what the application itself costs. */
    u32 total_clip = 0;
    for ( u32 ci = 0; ci < GUI_MAX_CLIP_RECTS; ++ci )
        total_clip += clip_used[ ci ];
    s_stats.accum.clip_count = total_clip;

    if ( s_caps.stats_trace && s_cache.any_changed )
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
        /* Too many distinct clips: emit in natural order (correct, just less merged).  Volatile
           ranges are naturally contiguous in this order, so no special handling is needed. */
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
        {
            /* Plain (non-volatile) commands of this clip group, in emission order -- these all
               merge into one GPU batch exactly as they always have (titlebar, scrollbar, text). */
            for ( u32 si = 0; si < nseg; ++si )
            {
                if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
                for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                    if ( s_draw.cmd_volatile_id[ i ] == GUI_ID_NONE
                      && s_draw.cmds[ i ].clip_idx == clip_ids[ g ] )
                        { win_font[ n ] = segs[ si ].font; win_order[ n++ ] = i; }
            }

            /* Volatile ranges anchored to this clip group, appended at the group's END.  A range
               must stay CONTIGUOUS in the permutation (tess_dispatch brackets one capture span
               per range), so it cannot be split across groups: the WHOLE range goes where its
               first visible command's clip belongs.  Appending after the group's plain commands
               (rather than in scan position) keeps those mergeable into a single batch on both
               sides of the widget; the block's own commands are forced separate regardless, since
               a patch must be able to rewrite their elem_counts.  The one trade: a block paints
               after same-clip sibling content of its window, so it must not rely on that content
               overdrawing it. */
            for ( u32 si = 0; si < nseg; ++si )
            {
                if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
                for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                {
                    gui_id_t vid = s_draw.cmd_volatile_id[ i ];
                    if ( vid == GUI_ID_NONE )
                        continue;
                    if ( i > segs[ si ].lo && s_draw.cmd_volatile_id[ i - 1 ] == vid )
                        continue;   /* not the range start; emitted with its range */

                    /* The range's first non-empty-clip command anchors it to a group; a fully
                       clip-empty range is emitted nowhere (matches its hidden state).  The
                       emitted subset uses the same empty-clip filter volatile_patch applies, so
                       capture and patch always tessellate the same command set. */
                    u32 hi = i;
                    while ( hi < segs[ si ].hi && s_draw.cmd_volatile_id[ hi ] == vid ) ++hi;
                    u32 anchor = i;
                    while ( anchor < hi
                            && rect_empty( s_draw.clip_table[ s_draw.cmds[ anchor ].clip_idx ] ) )
                        ++anchor;
                    if ( anchor == hi || s_draw.cmds[ anchor ].clip_idx != clip_ids[ g ] )
                        continue;

                    for ( u32 j = i; j < hi; ++j )
                        if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ j ].clip_idx ] ) )
                            { win_font[ n ] = segs[ si ].font; win_order[ n++ ] = j; }
                }
            }
        }
    }

    tess_dispatch( s_draw.cmds, win_order, win_font, n, win );
}

/*==============================================================================================
    Debug diagnostics -- dump the cached-geometry slot table, and a per-frame layout guard.

    cache_dump_slots prints every slot's window, z/vp, and its vertex / index / command bounds.
    Exposed to hosts via gui()->debug_dump_geometry() for on-demand inspection, and printed
    automatically by cache_validate_geometry right before it trips an assert.

    cache_validate_geometry enforces the ONE invariant the retained cache lives or dies by: every
    valid slot owns a DISJOINT reserved vertex and index range.  Slots are packed end-to-end in the
    shared s_tess buffers, so any overlap means a write into one slot (a tessellation, a reuse
    replay, or a volatile patch) silently corrupts another's live geometry -- the exact failure that
    made the hover tooltip flicker/warp when a neighbouring volatile widget patched over it.  These
    bugs are near-invisible after the fact (the command metadata still looks correct; only the
    vertex bytes are wrong), so the guard runs every frame in debug builds and traps at the source.
    Compiled to nothing in release (ORB_ASSERT is a no-op there; the whole call is gated too).
==============================================================================================*/

static void
cache_dump_slots( const char* tag )
{
    printf( "[gui] cached geometry [%s]: %u slots  tess v=%u/%u i=%u/%u c=%u/%u\n",
            tag ? tag : "dump", s_slot_count, s_tess.vert_count, GUI_MAX_VERTS,
            s_tess.idx_count, GUI_MAX_IDX, s_tess.cmd_count, GUI_MAX_CMDS );
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        const win_geo_slot_t* s = &s_slots[ i ];
        printf( "  [%2u] win=%-11u z=%-4u vp=%u  vert[%u..%u)/%u  idx[%u..%u)/%u  cmd[%u..%u)  gen=%u%s\n",
                i, s->win, s->z, s->vp,
                s->vert_base, s->vert_base + s->vert_count, s->vert_alloc,
                s->idx_base,  s->idx_base  + s->idx_count,  s->idx_alloc,
                s->cmd_base,  s->cmd_base  + s->cmd_count,  s->tess_gen,
                s->valid ? "" : "  (INVALID)" );
    }
}

#if !RELEASE
static void
cache_validate_geometry( void )
{
    for ( u32 a = 0; a < s_slot_count; ++a )
    {
        const win_geo_slot_t* sa = &s_slots[ a ];
        if ( !sa->valid )
            continue;

        /* Live geometry must fit inside the reservation it was measured into. */
        if ( sa->vert_count > sa->vert_alloc || sa->idx_count > sa->idx_alloc )
            cache_dump_slots( "OVERFLOW" );
        ORB_ASSERT_MSG( sa->vert_count <= sa->vert_alloc,
                        "gui cache: slot live vertex count exceeds its reservation" );
        ORB_ASSERT_MSG( sa->idx_count <= sa->idx_alloc,
                        "gui cache: slot live index count exceeds its reservation" );

        u32 av0 = sa->vert_base, av1 = sa->vert_base + sa->vert_alloc;
        u32 ai0 = sa->idx_base,  ai1 = sa->idx_base  + sa->idx_alloc;

        /* No two slots may share buffer space (adjacency at a shared boundary is fine). */
        for ( u32 b = a + 1; b < s_slot_count; ++b )
        {
            const win_geo_slot_t* sb = &s_slots[ b ];
            if ( !sb->valid )
                continue;
            u32  bv0 = sb->vert_base, bv1 = sb->vert_base + sb->vert_alloc;
            u32  bi0 = sb->idx_base,  bi1 = sb->idx_base  + sb->idx_alloc;
            bool vhit = ( av0 < bv1 && bv0 < av1 );
            bool ihit = ( ai0 < bi1 && bi0 < ai1 );
            if ( vhit || ihit )
                cache_dump_slots( "OVERLAP" );
            ORB_ASSERT_MSG( !vhit, "gui cache: two window slots share vertex-buffer space" );
            ORB_ASSERT_MSG( !ihit, "gui cache: two window slots share index-buffer space" );
        }
    }
}
#endif

/* Host entry point (gui()->debug_dump_geometry): print the current slot table on demand. */
void
gui_build_dump_geometry( void )
{
    cache_dump_slots( "manual" );
}

/* Reuse a window's geometry in place: it sits at prev->vert_base unchanged, so copy the slot fields,
   advance the write head by the padded alloc (keeping the next window's prev->vert_base aligned with
   s_tess.vert_count), and replay its cached GPU commands at this frame's offsets. */
static void
cache_slot_reuse( win_geo_slot_t* slot, win_geo_slot_t* prev, u32 wi )
{
    /* Geometry is in place at prev->vert_base.  Advance the write head by the padded
       alloc so the next window's prev->vert_base aligns with s_tess.vert_count. */
    slot->vert_base  = prev->vert_base;
    slot->vert_count = prev->vert_count;
    slot->vert_alloc = prev->vert_alloc;
    slot->idx_base   = prev->idx_base;
    slot->idx_count  = prev->idx_count;
    slot->idx_alloc  = prev->idx_alloc;
    slot->tess_gen   = prev->tess_gen;   /* geometry unchanged: same tessellation pass */
    s_tess.vert_count = prev->vert_base + prev->vert_alloc;
    s_tess.idx_count  = prev->idx_base  + prev->idx_alloc;

    /* Replay GPU commands from the stable cache.  lvbase/libase are slot-local, so
       adding slot->vert_base/idx_base gives the absolute offsets for this frame. */
    slot->cmd_base  = s_tess.cmd_count;
    slot->cmd_count = s_win_cached_count[ wi ];
    u32 nc = slot->cmd_count;
    for ( u32 k = 0; k < nc; ++k )
    {
        u32 ci = slot->cmd_base + k;
        s_tess.cmds    [ ci ]  = s_win_cached[ wi ][ k ].cmd;
        s_tess.cmd_vp  [ ci ]  = s_win_cached[ wi ][ k ].vp;
        s_tess.cmd_vbase[ ci ] = slot->vert_base + s_win_cached[ wi ][ k ].lvbase;
        s_tess.cmd_ibase[ ci ] = slot->idx_base  + s_win_cached[ wi ][ k ].libase;
    }
    s_tess.cmd_count += nc;

    /* This window's own content matched, but that comparison excludes volatile commands entirely
       (cache_diff_windows).  Its volatile rows are patched from this frame's live emit AFTER the
       loop (see reused_volatile_wins) -- once every slot is placed and s_tess.vert_count is the true
       tail, so the patch's scratch tessellation cannot land on a later slot's live geometry.  The
       slot fields (incl. tess_gen) set here are what volatile_patch resolves and generation-checks
       against then. */
    slot->valid = true;
}

/* Tessellate a changed / new / unstable window at the current write head (which, in stable mode,
   equals prev->vert_base, so this overwrites only the window's own prior region).  Keeps the padded
   reservation if the new geometry fits so downstream slots stay put; otherwise expands and
   invalidates all downstream prev slots so they re-tessellate at their shifted homes.  Finally
   writes the GPU commands into the stable cache for reuse next retained frame. */
static void
cache_slot_tessellate( win_geo_slot_t* slot, const render_win_hash_t* wh, win_geo_slot_t* prev,
                       u32 wi, bool set_stable )
{
    slot->vert_base       = s_tess.vert_count;
    slot->idx_base        = s_tess.idx_count;
    slot->cmd_base        = s_tess.cmd_count;
    slot->tess_gen        = ++s_tess_gen_next;   /* fresh pass: volatile captures bind to it */
    s_tess.slot_vert_base = s_tess.vert_count;
    s_tess.slot_idx_base  = s_tess.idx_count;
    s_tess.slot_cmd_base  = s_tess.cmd_count;
    s_tess.slot_tess_gen  = slot->tess_gen;
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
        s_win_cached[ wi ][ k ].libase = s_tess.cmd_ibase[ ci ] - slot->idx_base;
    }
    slot->cmd_count = nc;
    slot->valid     = true;
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

    /* Command-stepper capture: the frame's segments are closed and every emit pool is complete,
       nothing is diffed or tessellated yet -- the exact seam to freeze the live command list.
       A no-op unless GUI_CMD_STEPPER and a capture was requested (gui_step_capture). */
    STEP_CAPTURE_BUILD();

    /* Text-selection run capture: same seam.  Rebuilds the selection run buffer for any window
       marked GUI_WIN_TEXT_SELECT this frame; a two-branch no-op while no flagged window is
       live.  See backend/gui_select_capture.c. */
    select_capture_build();

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
    s_tess.band0_vert_end = 0;   /* re-derived below as main-band slots place; 0 when none exist */
    s_tess.band0_idx_end  = 0;

    /* set_stable: the window set is the same as last frame (count, order, all valid).
       When true, each cur[wi] aligns with slots_prev[wi] by the sorted-by-win invariant,
       and unchanged geometry is in-place at prev->vert_base. */
    bool set_stable = ( s_slot_prev_count == s_cache.cur_n );
    for ( u32 wi = 0; set_stable && wi < s_cache.cur_n; ++wi )
        if ( s_slots_prev[ wi ].win != s_cache.cur[ wi ].win || !s_slots_prev[ wi ].valid )
            set_stable = false;

    /* No global "reflow generation" is needed for volatile rows here: a row resolves its window's
       CURRENT slot by id at patch time (cache_slot_lookup) and its per-slot tess_gen check refuses
       any slot whose geometry it did not capture -- a moved or rebuilt slot can never be patched
       at a stale offset by construction. */

    u32 vert_retained = 0, tri_retained = 0, win_retained = 0;
    u32 total_vert    = 0, total_tri    = 0, overlay_win  = 0;

    /* First window whose tessellation exhausts the arena -- captured so the overflow report below
       can name the culprit ("window X overflowed at N/CAP") instead of a bare "something overflowed".
       overflow_at_* record the fill level reached when it first hit (the reservation drops the
       primitive that would not fit, so the counts are what actually made it in). */
    gui_id_t overflow_win     = GUI_ID_NONE;
    u32      overflow_at_vert = 0, overflow_at_idx = 0, overflow_at_cmd = 0;

    /* Windows reused this frame that carry volatile rows: their reserved regions are patched AFTER
       the whole slot loop completes.  Patching cannot run inline in the reuse branch because
       volatile_patch tessellates into scratch at s_tess.vert_count -- which, mid-loop, is only the
       write head up to the CURRENT window, with later windows' still-valid geometry sitting above
       it.  Patching a reused window whose slot is followed by a not-yet-repositioned reused window
       (e.g. a tooltip that outsorts it by id) would scribble scratch straight through that
       neighbour's live vertices, and if the neighbour is itself reused (never re-tessellated) the
       corruption reaches the screen.  Deferring until the loop ends puts s_tess.vert_count at the
       true tail, past every slot, so the scratch is always clear of live geometry. */
    gui_id_t reused_volatile_wins[ RENDER_MAX_WIN ];
    u32      reused_volatile_n = 0;

    /* Step 2: for each window, reuse geometry or re-tessellate, then register in dispatch. */
    for ( u32 wi = 0; wi < s_cache.cur_n; ++wi )
    {
        const render_win_hash_t* wh   = &s_cache.cur[ wi ];
        win_geo_slot_t*          slot = &s_slots[ s_slot_count++ ];
        win_geo_slot_t*          prev = set_stable ? &s_slots_prev[ wi ] : NULL;

        slot->win   = wh->win;
        slot->z     = wh->z;    // max segment z, pre-computed in cache_diff_windows
        slot->vp    = wh->vp;   // last segment vp, pre-computed in cache_diff_windows
        slot->band  = wh->band; // arena band; band-major sort already placed debug slots last
        slot->valid = false;

        bool reuse_geo = set_stable && s_caps.retained_cache && !wh->changed && prev->valid;

        bool ovf_before = s_tess.overflow;   /* did the arena already spill before this window? */

        if ( reuse_geo )
        {
            cache_slot_reuse( slot, prev, wi );

            /* Register for the deferred volatile patch after the loop, and tally retained
               geometry (debug-band windows are excluded from the totals below). */
            if ( reused_volatile_n < RENDER_MAX_WIN )
                reused_volatile_wins[ reused_volatile_n++ ] = wh->win;

            if ( wh->band == 0 )
            {
                vert_retained += slot->vert_count;
                tri_retained  += slot->idx_count / 3u;
                ++win_retained;
            }
        }
        else
        {
            cache_slot_tessellate( slot, wh, prev, wi, set_stable );
        }

        /* First window to tip the arena over: remember it (and the fill it reached) for the report. */
        if ( !ovf_before && s_tess.overflow && overflow_win == GUI_ID_NONE )
        {
            overflow_win     = wh->win;
            overflow_at_vert = s_tess.vert_count;
            overflow_at_idx  = s_tess.idx_count;
            overflow_at_cmd  = s_tess.cmd_count;
        }

        /* Accumulate per-slot geometry stats; exclude self-measuring debug-band windows from totals. */
        if ( wh->band != 0 )
            ++overlay_win;
        else
        {
            total_vert += slot->vert_count;
            total_tri  += slot->idx_count / 3u;

            /* Band boundary: the write head after the last main-band slot (band-major order puts
               them all first).  The dashboard's memory map draws "main arena ends here" at this
               mark; everything past it is the debug band's own footprint. */
            s_tess.band0_vert_end = s_tess.vert_count;
            s_tess.band0_idx_end  = s_tess.idx_count;
        }

        s_dispatch[ s_dispatch_count++ ] = slot;
    }

    /* Deferred volatile patches for reused windows: every slot is now placed and s_tess.vert_count
       is the true tail, so volatile_patch's scratch tessellation lands past all live geometry
       instead of over a later slot's vertices (see reused_volatile_wins above). */
    for ( u32 i = 0; i < reused_volatile_n; ++i )
        cache_count_volatile_patch( volatile_patch_reused_window( reused_volatile_wins[ i ] ) );

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

    /* Debug guard: assert the slot layout is disjoint before any of it reaches the GPU. */
#if !RELEASE
    cache_validate_geometry();
#endif

    /* Publish geometry and retained stats. */
    s_stats.accum.vert_count    = total_vert;
    s_stats.accum.tri_count     = total_tri;
    s_stats.accum.win_total     = s_cache.cur_n - overlay_win;
    s_stats.accum.win_retained  = win_retained;
    s_stats.accum.vert_retained = vert_retained;
    s_stats.accum.tri_retained  = tri_retained;

    /* Track geometry high-water marks and warn once on overflow.  The total (both bands) and the
       main band alone peak independently, so each gets its own accumulator. */
    if ( s_tess.vert_count     > s_tess.vert_hwm       ) s_tess.vert_hwm      = s_tess.vert_count;
    if ( s_tess.idx_count      > s_tess.idx_hwm        ) s_tess.idx_hwm       = s_tess.idx_count;
    if ( s_tess.band0_vert_end > s_tess.band0_vert_hwm ) s_tess.band0_vert_hwm = s_tess.band0_vert_end;
    if ( s_tess.band0_idx_end  > s_tess.band0_idx_hwm  ) s_tess.band0_idx_hwm  = s_tess.band0_idx_end;

    bool check_for_overflow = true;
    if ( check_for_overflow )
    {
        /* Single overflow catch for the whole build: the reservation sites just latch s_tess.overflow
           and drop their primitive (non-fatal -- the frame still submits everything that fit, the app
           keeps running), so we report ONCE here, after the frame is fully tessellated and about to be
           submitted.  We name the window that blew the caps (log line + break-once assert below) so a
           dropped primitive -- classically a window's late-tessellated chrome vanishing -- is traced to
           its source; the log and the dashboard's OVERFLOWED marker persist even past the assert. */
        /* Spill outside the per-window loop (a deferred volatile patch): no culprit window was captured,
           so report the final arena fill rather than a stale zero. */
        if ( s_tess.overflow && overflow_win == GUI_ID_NONE )
        {
            overflow_at_vert = s_tess.vert_count;
            overflow_at_idx  = s_tess.idx_count;
            overflow_at_cmd  = s_tess.cmd_count;
        }

        if ( s_tess.overflow && !s_tess.overflow_ever )
        {
            /* Name the window that hit the wall (a title in debug, else its hashed id) plus the fill it
               reached, so the report points at the culprit instead of just "something overflowed".  A
               NONE id means the spill happened outside the per-window loop (a deferred volatile patch). */
            const char* nm = ( overflow_win != GUI_ID_NONE ) ? gui_debug_name( overflow_win ) : NULL;
            printf( "[gui] WARNING: draw list overflow -- geometry dropped tessellating window '%s' "
                    "(id 0x%08X); arena filled to %u/%u verts, %u/%u idx, %u/%u gpu cmds. "
                    "Raise GUI_MAX_VERTS / GUI_MAX_IDX / GUI_MAX_CMDS.\n",
                    nm ? nm : "<unnamed>", (unsigned)overflow_win,
                    overflow_at_vert, GUI_MAX_VERTS, overflow_at_idx, GUI_MAX_IDX,
                    overflow_at_cmd, GUI_MAX_CMDS );
            fflush( stdout );   /* flush the diagnostic before the once-assert below can trap */
        }
        if ( s_tess.overflow )
             s_tess.overflow_ever = true;

        /* Break once (debug) so you can catch which frame / UI blew the caps under the debugger; the
           macro self-latches, so a persistent overflow does not re-trap every frame.  Non-fatal: skip
           past it (or a release build) and the app keeps running with the dropped geometry. */
        ORB_ASSERT_MSG_ONCE( !s_tess.overflow, "gui draw list overflow -- geometry dropped; raise "
                                               "GUI_MAX_VERTS / GUI_MAX_IDX / GUI_MAX_CMDS (gui.h)" );
    }

    /* Debug trace: print the current geometry counts only when they change (spam prevention)
       The peak high-water marks are always printed  */

    static u32 prev_verts = ~0u, prev_idx = ~0u;
    if ( s_caps.stats_trace && ( s_tess.vert_count != prev_verts || s_tess.idx_count != prev_idx ) )
    {
        printf( "[gui] geometry: verts %u/%u (peak %u)  idx %u/%u (peak %u)\n",
                s_tess.vert_count, GUI_MAX_VERTS, s_tess.vert_hwm,
                s_tess.idx_count,  GUI_MAX_IDX,   s_tess.idx_hwm );
        prev_verts = s_tess.vert_count;
        prev_idx   = s_tess.idx_count;
    }

    /* Debug trace: print the retained cache counts only when they change (spam prevention)
       The peak high-water marks are always printed  */

    static u32 prev_win_ret = ~0u, prev_vert_ret = ~0u;
    if ( s_caps.stats_trace &&
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

    /* Pipeline-dashboard snapshot: everything above is final for the frame -- slots placed,
       dispatch z-sorted, stats accumulated.  A no-op unless GUI_PIPELINE_DASHBOARD. */
    DASH_CAPTURE_BUILD();
}

// clang-format on
/*============================================================================================*/
