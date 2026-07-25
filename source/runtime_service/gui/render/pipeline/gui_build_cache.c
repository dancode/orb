/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_cache.c -- Retained frame-geometry cache (BUILD phase).

    The render pipeline has three phases.  This file is the middle one:

        EMIT   (gui_emit_draw.c)  widgets push semantic shapes -> s_draw command list,
                                   cut into per-(win,z,vp,font,band) segments, one hash baked per command.
        BUILD  (this file)        once per frame: diff each window's commands against last frame,
                                   reuse unchanged geometry in place, tessellate changed windows,
                                   then z-sort the result into a dispatch table.
        RENDER (gui_submit.c)     once per surface: upload changed geometry and emit one indexed
                                   draw call per cached GPU command.

    BUILD runs lazily on the first surface flush (cache_build_frame, guarded by s_frame_built)
    because the semantic command list is shared across every surface -- the geometry it produces
    is surface-independent.  gui_build_frame_reset clears the guard at frame_begin.

==============================================================================================*/
// clang-format off

/* Reverse id->name lookup, defined later in this unit (gui_debug_overlay.c) -- used by the overflow
   report below to name the window that blew the geometry caps.  Returns the registered title in
   debug builds (windows register via DBG_NAME in window_begin_ex), NULL when the name registry is
   compiled out (release) or the id was never registered. */
const char* gui_debug_name( gui_id_t id );

/*==============================================================================================
    Once-per-frame guard.

    The first surface flush triggers cache_build_frame and stamps s_frame_built; later surfaces
    reuse the slot and dispatch tables untouched.
==============================================================================================*/

static bool s_frame_built;

void
gui_build_frame_reset( void )
{
    s_frame_built = false;
}

/*==============================================================================================
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
==============================================================================================*/

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

/*==============================================================================================
    Retained-skip toggle.

    When enabled (default) a window whose per-command hash matches last frame keeps its geometry
    in place instead of re-tessellating.  Disable to benchmark or verify the from-scratch path.
    Toggled via gui()->set_retained_skip (key C in sb_vulkan).
==============================================================================================*/

static bool s_retained_cache = true;

void gui_build_set_retained_skip( bool on ) { s_retained_cache = on; }
bool gui_build_retained_skip    ( void )    { return s_retained_cache; }

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

/* RENDER_MAX_WIN / SLOT_VERT_PAD / SLOT_IDX_PAD live in gui_render.h (the dashboard snapshot
   types are sized by them). */
/* Max GPU draw commands cached per slot; most windows have 2-4, but every volatile block adds
   its own commands + reserved dormant pads (unmergeable across the reservation seams), so a
   window dense with volatile widgets multiplies fast.  A window that exceeds the cap is NOT
   truncated -- it goes uncacheable (see cache_slot_tessellate) and re-tessellates every real
   frame; the cap trades stable-cache memory against how large a window can be and still be
   retained. */
#ifdef GUI_STRESS_TEST
#define WIN_SLOT_CMD_MAX  128   /* stress-bench build: volatile swarms are a load axis */
#else
#define WIN_SLOT_CMD_MAX  64
#endif

/* Proactive compaction threshold.  The reuse allocator is bump-only: new windows always tessellate
   at the tail (past every live reservation), so a closed/relocated window's space becomes a hole
   nothing refills -- a churny UI (dragging across a menu bar, each submenu a fresh window that
   leaves a hole when it closes) marches the tail up and strands dead space behind it.  Waiting for
   the arena to hit the cap before repacking means the fill only ever grows during churn.  So once
   the DEAD (unreserved) space in the used range [0, tail) reaches this percentage, cache_build_frame
   repacks proactively -- the backend self-compacts on a cheap frame instead of on a user event or a
   hard overflow.  Padding (vert_alloc - vert_count) is NOT counted as dead, so a freshly repacked
   arena reads ~0% and the trigger cannot thrash frame-to-frame. */
#define GUI_REPACK_FRAG_PCT   25
#define GUI_REPACK_FRAG_FLOOR 2048   /* skip the check below this many dead verts/idx -- noise */

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
    u32      z;
    u32      vert_base, vert_count, vert_alloc;  // VB: absolute position, actual count, padded reservation
    u32      idx_base,  idx_count,  idx_alloc;   // IB: absolute position, actual count, padded reservation
    u32      cmd_base,  cmd_count;               // range into s_tess.gpu_cmds[] for this window
    u32      tess_gen;                           // generation of the tess pass that produced the geometry
    u8       vp;                                 // viewport (GUI_MAX_VIEWPORTS = 4)
    u8       band;                               // arena band (0 = main UI, 1 = debug/diagnostic)
    bool     valid;                              // true once geometry has been tessellated at least once
    bool     cmd_cached;                         // command run fit the stable cache; false = the window
                                                 //   overflowed WIN_SLOT_CMD_MAX and must re-tessellate
                                                 //   every real frame instead of reusing (never truncate)

} win_geo_slot_t;

/* Stable GPU command cache -- outside the ping-pong arrays so the reuse path never copies it.
   ID-KEYED: each live window owns one entry for as long as it keeps appearing, independent of its
   sort position, so a window appearing or vanishing never invalidates its neighbours' cached runs.
   s_win_cached_live is the occupancy flag (win id 0 is a real key -- the background layer -- so a
   zero id cannot double as the free sentinel).  Entries are swept when their window leaves the
   frame (win_cache_sweep); live windows <= RENDER_MAX_WIN, so a live window always finds a slot.
   Written when a window tessellates; read every retained frame until the window changes again. */
static win_slot_cmd_t   s_win_cached      [ RENDER_MAX_WIN ][ WIN_SLOT_CMD_MAX ];
static u32              s_win_cached_count[ RENDER_MAX_WIN ];
static gui_id_t         s_win_cached_win  [ RENDER_MAX_WIN ];
static u8               s_win_cached_live [ RENDER_MAX_WIN ];

/* Resolve a window's cache entry: existing, else a freshly claimed free slot, else ~0u (only
   possible past RENDER_MAX_WIN live windows -- those are dropped from rendering anyway). */
static u32
win_cache_take( gui_id_t win )
{
    u32 free_c = ~0u;
    for ( u32 c = 0; c < RENDER_MAX_WIN; ++c )
    {
        if ( s_win_cached_live[ c ] && s_win_cached_win[ c ] == win )
            return c;
        if ( free_c == ~0u && !s_win_cached_live[ c ] )
            free_c = c;
    }
    if ( free_c != ~0u )
    {
        s_win_cached_live [ free_c ] = 1;
        s_win_cached_win  [ free_c ] = win;
        s_win_cached_count[ free_c ] = 0;
    }
    return free_c;
}

/* Find without claiming -- the reuse path's read-side lookup.  ~0u when absent. */
static u32
win_cache_find( gui_id_t win )
{
    for ( u32 c = 0; c < RENDER_MAX_WIN; ++c )
        if ( s_win_cached_live[ c ] && s_win_cached_win[ c ] == win )
            return c;
    return ~0u;
}

static u32              s_slot_count, s_slot_prev_count;
static win_geo_slot_t   s_slots_a  [ RENDER_MAX_WIN ];
static win_geo_slot_t   s_slots_b  [ RENDER_MAX_WIN ];
static win_geo_slot_t*  s_slots      = s_slots_a;   // current frame (write)
static win_geo_slot_t*  s_slots_prev = s_slots_b;   // previous frame (read)
static win_geo_slot_t*  s_dispatch [ RENDER_MAX_WIN ];
static u32              s_dispatch_count;

/* Volatile widgets (gui_volatile_cb_open/_stamp/_close, gui_update_volatile, the registry and
   volatile_patch) live in their own file -- render/pipeline/gui_build_volatile.c, included right
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
    u16      seg_head;       // this window's segment chain (via s_seg_next); SEG_CHAIN_END = empty.
    u16      seg_tail;       //   Built in pass 1 below so the tess pass walks only ITS segments
                             //   instead of rescanning the whole segment table per window.
    u8       vp;             // viewport of the last segment this frame (GUI_MAX_VIEWPORTS = 4)
    u8       band;           // arena band: sticky OR across segments (any debug seg = debug window)
    bool     changed;        // hash mismatched, window is new, or force_changed this frame
    bool     force_changed;  // a volatile row in this window needs a (re)capture -- tessellate
                             // regardless of the hash (which excludes volatile commands entirely)

} render_win_hash_t;

/* Per-window segment chain links, parallel to s_draw.segs (GUI_MAX_SEGS fits u16).  Rebuilt each
   frame by cache_diff_windows; only meaningful through the records' seg_head/seg_tail. */
#define SEG_CHAIN_END 0xFFFFu
static u16 s_seg_next[ GUI_MAX_SEGS ];

/* Band-major, win-minor record ordering.  Debug-band records place after every main-band record,
   so within any one placement pass their FRESH allocations land past the main band's -- the debug
   UI's per-frame churn stays out of the main band's way.  (Slots are id-keyed and keep their
   historical positions across frames, so this is a per-pass allocation-order property, not a
   byte-layout guarantee; the repack pass restores strict band-major packing.)  Both cur[] and
   prev[] sort with this same rule every frame so the diff below stays one linear scan. */
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
    u32 memo_bi   = ~0u;   /* last record hit -- consecutive segments usually share a window */
    u8  clip_used[ GUI_MAX_CLIP_RECTS ] = { 0 };   /* clip table entries a band-0 command references */
    for ( u32 si = 0; si < nseg; ++si )
    {
        if ( segs[ si ].lo == segs[ si ].hi ) continue;   // empty span

        gui_id_t win = segs[ si ].win;

        if ( segs[ si ].band == 0 )   /* debug-band UI never counts in the stats it displays */
            total_cmd += segs[ si ].hi - segs[ si ].lo;

        /* Find or create the per-window record.  The memo short-circuits the scan for the common
           run of consecutive same-window segments (a window's clip pushes / font swaps). */
        u32 bi;
        if ( memo_bi < s_cache.cur_n && s_cache.cur[ memo_bi ].win == win )
            bi = memo_bi;
        else
            for ( bi = 0; bi < s_cache.cur_n; ++bi )
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
                GUI_WARN_ONCE( "more than %u windows this frame -- extra windows "
                               "are not rendered. Raise RENDER_MAX_WIN.\n", RENDER_MAX_WIN );
                ORB_ASSERT_MSG_ONCE( false, "gui window overflow -- more than RENDER_MAX_WIN "
                                            "windows; extra windows dropped. Raise RENDER_MAX_WIN "
                                            "(gui_render.h)" );
                continue;
            }
            s_cache.cur[ bi ] = ( render_win_hash_t ){ .win  = win, .hash = 2166136261u,
                                                       .seg_head = SEG_CHAIN_END,
                                                       .seg_tail = SEG_CHAIN_END };
            ++s_cache.cur_n;
        }
        memo_bi = bi;

        /* Chain this segment onto the window's list, in scan (emission) order. */
        s_seg_next[ si ] = SEG_CHAIN_END;
        if ( s_cache.cur[ bi ].seg_tail == SEG_CHAIN_END )
            s_cache.cur[ bi ].seg_head = (u16)si;
        else
            s_seg_next[ s_cache.cur[ bi ].seg_tail ] = (u16)si;
        s_cache.cur[ bi ].seg_tail = (u16)si;

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

    /* Sweep the id-keyed GPU command cache: an entry whose window left the frame is freed so its
       slot is available to the next appearing window.  Live entries <= cur_n <= RENDER_MAX_WIN. */
    for ( u32 c = 0; c < RENDER_MAX_WIN; ++c )
    {
        if ( !s_win_cached_live[ c ] ) continue;
        bool live = false;
        for ( u32 i = 0; i < s_cache.cur_n; ++i )
            if ( s_cache.cur[ i ].win == s_win_cached_win[ c ] ) { live = true; break; }
        if ( !live )
        {
            s_win_cached_live [ c ] = 0;
            s_win_cached_count[ c ] = 0;
        }
    }

    s_stats.accum.cmd_count      = total_cmd;
    s_stats.accum.seg_count      = nseg;
    s_stats.accum.text_pool_used = s_draw.text_pool_used;   /* frame emit is complete at the build seam */

    /* Distinct clip rects the main band drew under this frame.  Counted by command reference, not
       raw table size (s_draw.clip_table_n): the raw table also holds debug-band pushes and pushes
       no surviving command references, so it overstates what the application itself costs. */
    u32 total_clip = 0;
    for ( u32 ci = 0; ci < GUI_MAX_CLIP_RECTS; ++ci )
        total_clip += clip_used[ ci ];
    s_stats.accum.clip_count = total_clip;
}

/*==============================================================================================
    Per-window tessellation (BUILD step 2 helper).

    Gathers a window's commands (via the segment chain cache_diff_windows built onto its diff
    record) and builds a clip-sorted permutation, then hands it to tess_dispatch.  Grouping by
    clip makes equal-clip shapes tessellate back-to-back so they can merge into one GPU draw call.

    The permutation is a COUNTING SORT by clip group: one walk collects the groups and counts
    each group's population, offsets are prefix-summed, and a second walk scatters the commands to
    their final positions.  The sort is STABLE -- commands keep their emission order within a
    group, because paint order inside one clip is load-bearing (a window's titlebar chrome
    overpaints the content scrolled under it; see walk 2).  Every walk touches only this window's
    own commands (the chain), so the cost is O(window cmds), not O(clip groups x all segments)
    as the old rescan was -- the difference is what a 20x window pays per changed frame.

    z is NOT sorted here -- a window occupies one slot whose dispatch z is its max segment z,
    keeping all of a window's geometry contiguous; cache_build_frame z-sorts the slots after.
==============================================================================================*/

#define RENDER_MAX_CLIP_GROUPS  64   // distinct clip indices groupable within one window

/* Group handle for a clip index: existing group, else a new one in first-seen order (the order
   the groups are laid out in).  Returns ~0u on group-table overflow.  The one-entry memo serves
   the common run of consecutive same-clip commands. */
typedef struct
{
    u8  clip_ids [ RENDER_MAX_CLIP_GROUPS ];
    u32 n_clips;
    u8  memo_ci;    /* last clip index resolved (0xFF = empty memo) */
    u32 memo_g;     /* its group */

} clip_groups_t;

static u32
clip_group_of( clip_groups_t* cg, u8 ci )
{
    if ( cg->memo_ci == ci )
        return cg->memo_g;
    for ( u32 g = 0; g < cg->n_clips; ++g )
        if ( cg->clip_ids[ g ] == ci )
        {
            cg->memo_ci = ci;
            cg->memo_g  = g;
            return g;
        }
    if ( cg->n_clips >= RENDER_MAX_CLIP_GROUPS )
        return ~0u;
    cg->clip_ids[ cg->n_clips ] = ci;
    cg->memo_ci = ci;
    cg->memo_g  = cg->n_clips;
    return cg->n_clips++;
}

/* Bounds of the volatile range starting at command i within segment span [i, seg_hi): *out_hi is
   one past the range, *out_anchor its first non-empty-clip command (== *out_hi when fully clip-
   empty -- a hidden range, emitted nowhere, matching volatile_patch's filter). */
static void
tess_volatile_range( u32 i, u32 seg_hi, gui_id_t vid, u32* out_hi, u32* out_anchor )
{
    u32 hi = i;
    while ( hi < seg_hi && s_draw.cmd_volatile_id[ hi ] == vid ) ++hi;
    u32 anchor = i;
    while ( anchor < hi && rect_empty( s_draw.clip_table[ s_draw.cmds[ anchor ].clip_idx ] ) )
        ++anchor;
    *out_hi     = hi;
    *out_anchor = anchor;
}

/* Permutation output scratch, reused by every cache_tess_window call (cache_build_frame is
   single-threaded and guarded against re-entry).  s_win_font[] carries the segment's font
   alongside each reordered index because the clip sort crosses segment boundaries -- the font
   is a per-segment property, not per-command, so it must travel with its commands.
   u16: order values are command indices (< GUI_MAX_CMDS, asserted u16-safe at gui_cmd_seg_t);
   font ids are registry slots (< GUI_FONT_REGISTRY_MAX). */
static u16 s_win_order[ GUI_MAX_CMDS ];
static u16 s_win_font [ GUI_MAX_CMDS ];

static void
cache_tess_window( const render_win_hash_t* wh )
{
    const gui_cmd_seg_t* segs = s_draw.segs;

    /* Walk 1 -- collect clip groups (first-seen order over every visible command, volatile
       included, so the group layout matches emission) and count each group's populations:
       plain commands by their own clip, volatile ranges by their anchor's clip (a range stays
       whole -- see the placement comment below). */
    clip_groups_t cg = { .n_clips = 0, .memo_ci = 0xFF };
    u32  grp_cnt[ RENDER_MAX_CLIP_GROUPS ] = { 0 };
    bool overflow = false;

    for ( u16 si = wh->seg_head; si != SEG_CHAIN_END && !overflow; si = s_seg_next[ si ] )
    {
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi && !overflow; ++i )
        {
            gui_id_t vid = s_draw.cmd_volatile_id[ i ];
            if ( vid == GUI_ID_NONE )
            {
                u8 ci = s_draw.cmds[ i ].clip_idx;
                if ( rect_empty( s_draw.clip_table[ ci ] ) ) continue;
                u32 g = clip_group_of( &cg, ci );
                if ( g == ~0u ) { overflow = true; break; }
                ++grp_cnt[ g ];
            }
            else
            {
                u32 hi, anchor;
                tess_volatile_range( i, segs[ si ].hi, vid, &hi, &anchor );
                if ( anchor < hi )
                {
                    /* Count every visible command of the range at its ANCHOR's group (the range
                       is placed whole there), while registering each visible clip it spans in
                       emission order so the group layout matches the old full scan. */
                    u32 ag = clip_group_of( &cg, s_draw.cmds[ anchor ].clip_idx );
                    if ( ag == ~0u ) { overflow = true; break; }
                    for ( u32 j = anchor; j < hi; ++j )
                    {
                        u8 ci = s_draw.cmds[ j ].clip_idx;
                        if ( rect_empty( s_draw.clip_table[ ci ] ) ) continue;
                        if ( clip_group_of( &cg, ci ) == ~0u ) { overflow = true; break; }
                        ++grp_cnt[ ag ];
                    }
                }
                i = hi - 1;   /* loop ++ lands one past the range */
            }
        }
    }

    u32 n = 0;

    if ( overflow )
    {
        /* Too many distinct clips: emit in natural order (correct, just less merged).  Volatile
           ranges are naturally contiguous in this order, so no special handling is needed. */
        for ( u16 si = wh->seg_head; si != SEG_CHAIN_END; si = s_seg_next[ si ] )
            for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
                    { s_win_font[ n ] = segs[ si ].font; s_win_order[ n++ ] = (u16)i; }
        tess_dispatch( s_draw.cmds, s_win_order, s_win_font, n, wh->win );
        return;
    }

    /* Prefix-sum the group offsets: one span per group, groups laid out in first-seen order. */
    u32 grp_off[ RENDER_MAX_CLIP_GROUPS ];
    for ( u32 g = 0; g < cg.n_clips; ++g )
    {
        grp_off[ g ] = n;  n += grp_cnt[ g ];
    }

    /* Walk 2 -- scatter.  Every command lands at its group's single cursor in EMISSION order,
       volatile ranges included: the permutation reorders across groups, never within one.
       Paint order within a clip group is load-bearing, which is why the volatile block cannot be
       appended at the end of its group (it was, and this is the bug that cost): a window shares
       ONE clip rect across its background, its scrolled content and its titlebar/border chrome
       (window_open_body, chrome/window/gui_window_free.c), and relies on the chrome window_end
       emits LAST to overpaint whatever scrolled under it.  A block hoisted past that chrome draws
       over the titlebar instead of under it.
         A range still lands WHOLE at its ANCHOR's group -- it must stay CONTIGUOUS in the
       permutation (tess_dispatch brackets one capture span per range) and so cannot be split
       across groups -- but at the position its first command occupied, not at the tail.  The cost
       is one extra batch per block whose group has plain commands on both sides of it (they can no
       longer merge through it); the block's own commands were always forced separate anyway, since
       a patch must be able to rewrite their elem_counts. */
    for ( u16 si = wh->seg_head; si != SEG_CHAIN_END; si = s_seg_next[ si ] )
    {
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            gui_id_t vid = s_draw.cmd_volatile_id[ i ];
            if ( vid == GUI_ID_NONE )
            {
                u8 ci = s_draw.cmds[ i ].clip_idx;
                if ( rect_empty( s_draw.clip_table[ ci ] ) ) continue;
                u32 g = clip_group_of( &cg, ci );
                s_win_font [ grp_off[ g ] ]   = segs[ si ].font;
                s_win_order[ grp_off[ g ]++ ] = (u16)i;
            }
            else
            {
                u32 hi, anchor;
                tess_volatile_range( i, segs[ si ].hi, vid, &hi, &anchor );
                if ( anchor < hi )   /* hidden (fully clip-empty) ranges are emitted nowhere */
                {
                    u32 g = clip_group_of( &cg, s_draw.cmds[ anchor ].clip_idx );
                    for ( u32 j = i; j < hi; ++j )
                        if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ j ].clip_idx ] ) )
                        {
                            s_win_font [ grp_off[ g ] ]   = segs[ si ].font;
                            s_win_order[ grp_off[ g ]++ ] = (u16)j;
                        }
                }
                i = hi - 1;
            }
        }
    }

    tess_dispatch( s_draw.cmds, s_win_order, s_win_font, n, wh->win );
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

/* Find a window's slot in the PREVIOUS frame's table by id.  The id-keyed replacement for the
   old positional (set_stable) alignment: a window keeps its reservation across frames purely by
   identity, so windows appearing or vanishing around it cannot strip it of reuse. */
static win_geo_slot_t*
slot_prev_find( gui_id_t win )
{
    for ( u32 i = 0; i < s_slot_prev_count; ++i )
        if ( s_slots_prev[ i ].win == win && s_slots_prev[ i ].valid )
            return &s_slots_prev[ i ];
    return NULL;
}

/* Reuse a window's geometry in place: it sits at prev->vert_base unchanged, so copy the slot
   fields, fold its reservation into the arena tail (max, not assignment -- slots are id-keyed
   and may sit anywhere below the tail), and replay its cached GPU commands at this frame's
   offsets.  `cache_idx` is the window's entry in the id-keyed stable command cache. */
static void
cache_slot_reuse( win_geo_slot_t* slot, const win_geo_slot_t* prev, u32 cache_idx )
{
    slot->vert_base  = prev->vert_base;
    slot->vert_count = prev->vert_count;
    slot->vert_alloc = prev->vert_alloc;
    slot->idx_base   = prev->idx_base;
    slot->idx_count  = prev->idx_count;
    slot->idx_alloc  = prev->idx_alloc;
    slot->tess_gen   = prev->tess_gen;   /* geometry unchanged: same tessellation pass */

    /* Keep the write head (== the arena tail every fresh tessellation targets) past this slot. */
    if ( prev->vert_base + prev->vert_alloc > s_tess.vert_count )
        s_tess.vert_count = prev->vert_base + prev->vert_alloc;
    if ( prev->idx_base + prev->idx_alloc > s_tess.idx_count )
        s_tess.idx_count = prev->idx_base + prev->idx_alloc;

    /* Replay GPU commands from the stable cache.  lvbase/libase are slot-local, so
       adding slot->vert_base/idx_base gives the absolute offsets for this frame. */
    slot->cmd_base  = s_tess.cmd_count;
    slot->cmd_count = s_win_cached_count[ cache_idx ];
    u32 nc = slot->cmd_count;
    for ( u32 k = 0; k < nc; ++k )
    {
        u32 ci = slot->cmd_base + k;
        s_tess.gpu_cmds[ ci ].cmd   = s_win_cached[ cache_idx ][ k ].cmd;
        s_tess.gpu_cmds[ ci ].vp    = s_win_cached[ cache_idx ][ k ].vp;
        s_tess.gpu_cmds[ ci ].vbase = slot->vert_base + s_win_cached[ cache_idx ][ k ].lvbase;
        s_tess.gpu_cmds[ ci ].ibase = slot->idx_base  + s_win_cached[ cache_idx ][ k ].libase;
    }
    s_tess.cmd_count += nc;

    /* This window's own content matched, but that comparison excludes volatile commands entirely
       (cache_diff_windows).  Its volatile rows are patched from this frame's live emit AFTER the
       loop (see reused_volatile_wins) -- once every slot is placed and s_tess.vert_count is the true
       tail, so the patch's scratch tessellation cannot land on a later slot's live geometry.  The
       slot fields (incl. tess_gen) set here are what volatile_patch resolves and generation-checks
       against then. */
    slot->cmd_cached = true;   /* reuse only runs when prev cached its full command run */
    slot->valid      = true;
}

/* Tessellate a changed / new window.  The output always lands at the arena TAIL -- the write head
   past every live reservation (prev and this frame's alike) -- so a tessellation can never write
   over another window's live geometry, whatever it turns out to measure.  Then:

     - fits its own previous reservation -> the geometry is memcpy'd back into that hole (indices
       are slot-relative, so only the slot bases and the absolute vbase/ibase shift) and
       the tail rewinds -- the steady interactive case (the focused window changing every frame)
       reuses its home and the arena layout is byte-stable.
     - outgrew it (or is new)           -> it keeps the tail position with a fresh reservation
       sized with GEOMETRIC headroom (count/4, floored at the fixed pads) so a steadily growing
       window relocates O(log growth) times, not per frame.  The old reservation becomes a hole;
       holes at the tail self-heal as the extent is re-derived per frame, interior ones persist
       until the repack fallback (cache_build_frame) compacts the arena.

   This replaces the old downstream-invalidation cascade: no other window is ever touched.
   Finally writes the GPU commands into the id-keyed stable cache for reuse next retained frame. */
static void
cache_slot_tessellate( win_geo_slot_t* slot, const render_win_hash_t* wh,
                       const win_geo_slot_t* prev, u32 cache_idx )
{
    u32 tail_v = s_tess.vert_count;   /* invariant: the write head is past every live reservation */
    u32 tail_i = s_tess.idx_count;

    slot->vert_base       = tail_v;
    slot->idx_base        = tail_i;
    slot->cmd_base        = s_tess.cmd_count;
    slot->tess_gen        = ++s_tess_gen_next;   /* fresh pass: volatile captures bind to it */
    s_tess.slot_vert_base = tail_v;
    s_tess.slot_idx_base  = tail_i;
    s_tess.slot_cmd_base  = s_tess.cmd_count;
    s_tess.slot_tess_gen  = slot->tess_gen;
    s_tess.force_new_cmd  = true;

    cache_tess_window( wh );

    slot->vert_count = s_tess.vert_count - slot->vert_base;
    slot->idx_count  = s_tess.idx_count  - slot->idx_base;
    slot->cmd_count  = s_tess.cmd_count  - slot->cmd_base;

    bool fits = ( prev && prev->valid
                  && slot->vert_count <= prev->vert_alloc
                  && slot->idx_count  <= prev->idx_alloc );
    if ( fits )
    {
        /* Relocate from the tail scratch back into the window's own reservation.  The spans are
           disjoint by the tail invariant (tail >= prev base + alloc), so memcpy is safe.  Indices
           are slot-relative and move verbatim; only the absolute per-command bases shift. */
        u32 dv = tail_v - prev->vert_base;
        u32 di = tail_i - prev->idx_base;
        if ( dv || di )
        {
            memcpy( &s_tess.verts  [ prev->vert_base ], &s_tess.verts  [ tail_v ],
                    slot->vert_count * sizeof( gui_draw_vert_t ) );
            memcpy( &s_tess.indices[ prev->idx_base ],  &s_tess.indices[ tail_i ],
                    slot->idx_count * sizeof( u16 ) );
            for ( u32 k = 0; k < slot->cmd_count; ++k )
            {
                s_tess.gpu_cmds[ slot->cmd_base + k ].vbase -= dv;
                s_tess.gpu_cmds[ slot->cmd_base + k ].ibase -= di;
            }
        }
        slot->vert_base  = prev->vert_base;
        slot->vert_alloc = prev->vert_alloc;
        slot->idx_base   = prev->idx_base;
        slot->idx_alloc  = prev->idx_alloc;

        /* Rewind the tail: the scratch bytes are abandoned, nothing references them. */
        s_tess.vert_count = tail_v;
        s_tess.idx_count  = tail_i;
    }
    else
    {
        /* Stays at the tail.  Geometric headroom: a quarter of the live size, floored at the
           fixed pads, so growth relocates logarithmically instead of once per grown frame. */
        u32 pad_v = slot->vert_count / 4u;  if ( pad_v < SLOT_VERT_PAD ) pad_v = SLOT_VERT_PAD;
        u32 pad_i = slot->idx_count  / 4u;  if ( pad_i < SLOT_IDX_PAD  ) pad_i = SLOT_IDX_PAD;
        slot->vert_alloc = slot->vert_count + pad_v;
        slot->idx_alloc  = slot->idx_count  + pad_i;

        s_tess.vert_count = slot->vert_base + slot->vert_alloc;
        s_tess.idx_count  = slot->idx_base  + slot->idx_alloc;
    }

    /* Write GPU commands into the stable cache for reuse next retained frame.  A run that exceeds
       the cache must NOT be truncated: dropping trailing commands blanks the window's deferred
       chrome this frame and, worse, COMPACTS the reuse replay -- the next slot's cmd_base lands
       right after the truncated run, so a volatile patch whose local_cmd_base lies past the cap
       rewrites a NEIGHBOUR window's command table (the >8-volatile-block flicker).  The window
       stays fully drawn and idle-patchable (its dormant pads are live in s_tess); it just goes
       uncacheable, re-tessellating every real frame until it shrinks back under the cap. */
    u32 nc = slot->cmd_count;
    if ( nc > WIN_SLOT_CMD_MAX || cache_idx == ~0u )
    {
        if ( cache_idx != ~0u )
            s_win_cached_count[ cache_idx ] = 0;
        slot->cmd_cached = false;
        slot->valid      = true;
        return;
    }
    s_win_cached_count[ cache_idx ] = nc;
    for ( u32 k = 0; k < nc; ++k )
    {
        u32 ci = slot->cmd_base + k;
        s_win_cached[ cache_idx ][ k ].cmd    = s_tess.gpu_cmds[ ci ].cmd;
        s_win_cached[ cache_idx ][ k ].vp     = s_tess.gpu_cmds[ ci ].vp;
        s_win_cached[ cache_idx ][ k ].lvbase = s_tess.gpu_cmds[ ci ].vbase - slot->vert_base;
        s_win_cached[ cache_idx ][ k ].libase = s_tess.gpu_cmds[ ci ].ibase - slot->idx_base;
    }
    slot->cmd_cached = true;
    slot->valid      = true;
}

/*==============================================================================================
    cache_build_frame (BUILD step 2) -- diff, reuse or re-tessellate per window, z-sort.

    Runs once per frame (guarded by s_frame_built).  Produces the geometry (s_tess.verts/indices),
    the per-window slot table, and the back-to-front dispatch order -- all surface-independent.

    ID-KEYED in-place geometry reuse:
      A window is matched to last frame's slot by its ID (slot_prev_find), not by table position,
      so a window appearing or vanishing anywhere in the set never strips its neighbours of reuse
      (the old all-or-nothing set_stable rule made every hover tooltip re-tessellate the world).
      Unchanged windows keep their reservation in place.  Changed windows tessellate at the arena
      TAIL (past every live reservation) and are memcpy'd back into their own hole when they still
      fit -- see cache_slot_tessellate.  A vanished window leaves a hole; tail holes self-heal
      (the extent is re-derived from live slots each frame), interior holes persist as
      fragmentation until the repack fallback below compacts everything.

    Repack fallback:
      If placement overflows the arena, the whole placement pass is re-run once with reuse
      disabled: every window tessellates sequentially from 0, compacting all fragmentation in a
      single (heavier) frame.  Only if THAT still overflows is geometry genuinely dropped and the
      overflow reported.
==============================================================================================*/

/* Accumulators one placement pass produces for the stats/report code after it. */
typedef struct
{
    u32      vert_retained, tri_retained, win_retained;
    u32      total_vert, total_tri, overlay_win;
    u32      reserved_vert, reserved_idx;   /* sum of vert_alloc/idx_alloc over ALL placed slots */
    gui_id_t overflow_win;
    u32      overflow_at_vert, overflow_at_idx, overflow_at_cmd;
    gui_id_t reused_volatile_wins[ RENDER_MAX_WIN ];
    u32      reused_volatile_n;

} cache_place_stats_t;

/* One placement pass over s_cache.cur[]: reuse or tessellate every window and register it in
   dispatch.  allow_reuse false is the repack mode -- prev slots are ignored entirely and the
   arena packs sequentially from 0.  Resets and refills s_slots / s_dispatch / s_tess, so it is
   safe to run twice in one build (the repack retry). */
static void
cache_place_slots( bool allow_reuse, cache_place_stats_t* st )
{
    memset( st, 0, sizeof( *st ) );
    st->overflow_win = GUI_ID_NONE;

    s_slot_count     = 0;
    s_dispatch_count = 0;

    tess_reset();
    s_tess_stats.band0_vert_end = 0;   /* re-derived below as main-band slots place; 0 when none exist */
    s_tess_stats.band0_idx_end  = 0;

    /* Seed the write head at the arena tail: past every live prev reservation, so a fresh
       tessellation can never land inside geometry a later window still wants to reuse.  In
       repack mode nothing is reused, so packing starts at 0. */
    if ( allow_reuse )
    {
        for ( u32 i = 0; i < s_slot_prev_count; ++i )
        {
            if ( !s_slots_prev[ i ].valid ) continue;
            u32 ve = s_slots_prev[ i ].vert_base + s_slots_prev[ i ].vert_alloc;
            u32 ie = s_slots_prev[ i ].idx_base  + s_slots_prev[ i ].idx_alloc;
            if ( ve > s_tess.vert_count ) s_tess.vert_count = ve;
            if ( ie > s_tess.idx_count  ) s_tess.idx_count  = ie;
        }
    }

    /* No global "reflow generation" is needed for volatile rows here: a row resolves its window's
       CURRENT slot by id at patch time (cache_slot_lookup) and its per-slot tess_gen check refuses
       any slot whose geometry it did not capture -- a moved or rebuilt slot can never be patched
       at a stale offset by construction. */

    for ( u32 wi = 0; wi < s_cache.cur_n; ++wi )
    {
        const render_win_hash_t* wh   = &s_cache.cur[ wi ];
        win_geo_slot_t*          slot = &s_slots[ s_slot_count++ ];
        win_geo_slot_t*          prev = allow_reuse ? slot_prev_find( wh->win ) : NULL;
        u32                      ci   = win_cache_take( wh->win );

        slot->win   = wh->win;
        slot->z     = wh->z;    // max segment z, pre-computed in cache_diff_windows
        slot->vp    = wh->vp;   // last segment vp, pre-computed in cache_diff_windows
        slot->band  = wh->band; // arena band; band-major sort already placed debug slots last
        slot->valid = false;

        /* prev->cmd_cached: a window whose command run overflowed the stable cache last frame has
           nothing to replay from -- it must re-tessellate even when its content hash matched. */
        bool reuse_geo = allow_reuse && s_retained_cache && !wh->changed
                      && prev && prev->cmd_cached && ci != ~0u;

        bool ovf_before = s_tess.overflow;   /* did the arena already spill before this window? */

        if ( reuse_geo )
        {
            cache_slot_reuse( slot, prev, ci );

            /* Register for the deferred volatile patch after the loop, and tally retained
               geometry (debug-band windows are excluded from the totals below). */
            if ( st->reused_volatile_n < RENDER_MAX_WIN )
                st->reused_volatile_wins[ st->reused_volatile_n++ ] = wh->win;

            if ( wh->band == 0 )
            {
                st->vert_retained += slot->vert_count;
                st->tri_retained  += slot->idx_count / 3u;
                ++st->win_retained;
            }
        }
        else
        {
            cache_slot_tessellate( slot, wh, prev, ci );
        }

        /* First window to tip the arena over: remember it (and the fill it reached) for the report. */
        if ( !ovf_before && s_tess.overflow && st->overflow_win == GUI_ID_NONE )
        {
            st->overflow_win     = wh->win;
            st->overflow_at_vert = s_tess.vert_count;
            st->overflow_at_idx  = s_tess.idx_count;
            st->overflow_at_cmd  = s_tess.cmd_count;
        }

        /* Space this slot owns (live count + retained padding).  Summed over every slot, the tail
           minus this is the dead space the proactive-repack check reclaims. */
        st->reserved_vert += slot->vert_alloc;
        st->reserved_idx  += slot->idx_alloc;

        /* Accumulate per-slot geometry stats; exclude self-measuring debug-band windows from totals. */
        if ( wh->band != 0 )
            ++st->overlay_win;
        else
        {
            st->total_vert += slot->vert_count;
            st->total_tri  += slot->idx_count / 3u;

            /* Band boundary: the far edge of the main band's reservations (band-major sort placed
               them first, but id-keyed slots keep historical positions, so track the max extent
               rather than the loop's write head).  The dashboard's memory map draws "main arena
               ends here" at this mark; everything past it is the debug band's own footprint. */
            u32 ve = slot->vert_base + slot->vert_alloc;
            u32 ie = slot->idx_base  + slot->idx_alloc;
            if ( ve > s_tess_stats.band0_vert_end ) s_tess_stats.band0_vert_end = ve;
            if ( ie > s_tess_stats.band0_idx_end  ) s_tess_stats.band0_idx_end  = ie;
        }

        s_dispatch[ s_dispatch_count++ ] = slot;
    }
}

static void
cache_build_frame( void )
{
    if ( s_frame_built )
        return;
    s_frame_built = true;

    /* Close the still-open final segment so diff and tess see its full [lo, hi) range. */
    if ( s_draw.seg_count > 0 )
        s_draw.segs[ s_draw.seg_count - 1 ].hi = (u16)s_draw.cmd_count;

    /* Command-stepper capture: the frame's segments are closed and every emit pool is complete,
       nothing is diffed or tessellated yet -- the exact seam to freeze the live command list.
       A no-op unless GUI_CMD_STEPPER and a capture was requested (gui_step_capture). */
    STEP_CAPTURE_BUILD();

    /* Text-selection run capture: same seam.  Rebuilds the selection run buffer for any window
       marked GUI_WIN_TEXT_SELECT this frame; a two-branch no-op while no flagged window is
       live.  See render/gui_select_capture.c. */
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

    /* Step 2: place every window (reuse or re-tessellate).  An overflow on the reuse path may be
       pure fragmentation (holes from vanished / relocated windows), so retry ONCE as a repack --
       reuse off, sequential packing from 0 compacts the arena in a single heavier frame.  Only
       an overflow that survives the repack means the content genuinely exceeds the arena. */
    cache_place_stats_t ps;
    cache_place_slots( true, &ps );

    /* Repack triggers, both handled by a single from-scratch re-place (reuse off, pack from 0):
         1. Overflow -- the reuse path spilled the arena.  Often pure fragmentation (holes from
            vanished / relocated windows), so a compacting retry usually fits; an overflow that
            SURVIVES the repack means the content genuinely exceeds the arena.
         2. Proactive (GUI_REPACK_FRAG_PCT) -- the bump allocator never refills holes, so dead space
            only grows during churn.  Measure it (tail minus what live slots actually reserve) and
            compact once it crosses the threshold, so the backend self-heals on a cheap frame
            instead of waiting for a user event or a hard overflow.  Floored so a near-empty arena
            never trips on a high percentage of a tiny number. */
    u32  dead_v = s_tess.vert_count > ps.reserved_vert ? s_tess.vert_count - ps.reserved_vert : 0;
    u32  dead_i = s_tess.idx_count  > ps.reserved_idx  ? s_tess.idx_count  - ps.reserved_idx  : 0;
    bool frag   = ( dead_v >= GUI_REPACK_FRAG_FLOOR && dead_v * 100u >= s_tess.vert_count * GUI_REPACK_FRAG_PCT )
               || ( dead_i >= GUI_REPACK_FRAG_FLOOR && dead_i * 100u >= s_tess.idx_count  * GUI_REPACK_FRAG_PCT );

    if ( s_tess.overflow || frag )
        cache_place_slots( false, &ps );

    /* Deferred volatile patches for reused windows: every slot is now placed and s_tess.vert_count
       is the true tail, so volatile_patch's scratch tessellation lands past all live geometry
       instead of over a later slot's vertices (see reused_volatile_wins above). */
    for ( u32 i = 0; i < ps.reused_volatile_n; ++i )
        cache_count_volatile_patch( volatile_patch_reused_window( ps.reused_volatile_wins[ i ] ) );

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
    s_stats.accum.vert_count    = ps.total_vert;
    s_stats.accum.tri_count     = ps.total_tri;
    s_stats.accum.win_total     = s_cache.cur_n - ps.overlay_win;
    s_stats.accum.win_retained  = ps.win_retained;
    s_stats.accum.vert_retained = ps.vert_retained;
    s_stats.accum.tri_retained  = ps.tri_retained;

    /* Track geometry high-water marks and warn once on overflow.  The total (both bands) and the
       main band alone peak independently, so each gets its own accumulator. */
    if ( s_tess.vert_count           > s_tess_stats.vert_hwm       ) s_tess_stats.vert_hwm       = s_tess.vert_count;
    if ( s_tess.idx_count            > s_tess_stats.idx_hwm        ) s_tess_stats.idx_hwm        = s_tess.idx_count;
    if ( s_tess_stats.band0_vert_end > s_tess_stats.band0_vert_hwm ) s_tess_stats.band0_vert_hwm = s_tess_stats.band0_vert_end;
    if ( s_tess_stats.band0_idx_end  > s_tess_stats.band0_idx_hwm  ) s_tess_stats.band0_idx_hwm  = s_tess_stats.band0_idx_end;

    /* Single overflow catch for the whole build: the reservation sites just latch s_tess.overflow
       and drop their primitive (non-fatal -- the frame still submits everything that fit, the app
       keeps running), so we report ONCE here, after the frame is fully tessellated and about to be
       submitted.  We name the window that blew the caps (log line + break-once assert below) so a
       dropped primitive -- classically a window's late-tessellated chrome vanishing -- is traced to
       its source; the log and the dashboard's OVERFLOWED marker persist even past the assert. */
    /* Spill outside the per-window loop (a deferred volatile patch): no culprit window was captured,
       so report the final arena fill rather than a stale zero. */
    if ( s_tess.overflow && ps.overflow_win == GUI_ID_NONE )
    {
        ps.overflow_at_vert = s_tess.vert_count;
        ps.overflow_at_idx  = s_tess.idx_count;
        ps.overflow_at_cmd  = s_tess.cmd_count;
    }

    if ( s_tess.overflow && !s_tess_stats.overflow_ever )
    {
        /* Name the window that hit the wall (a title in debug, else its hashed id) plus the fill it
           reached, so the report points at the culprit instead of just "something overflowed".  A
           NONE id means the spill happened outside the per-window loop (a deferred volatile patch). */
        const char* nm = ( ps.overflow_win != GUI_ID_NONE ) ? gui_debug_name( ps.overflow_win ) : NULL;
        printf( "[gui] WARNING: draw list overflow -- geometry dropped tessellating window '%s' "
                "(id 0x%08X); arena filled to %u/%u verts, %u/%u idx, %u/%u gpu cmds. "
                "Raise GUI_MAX_VERTS / GUI_MAX_IDX / GUI_MAX_CMDS.\n",
                nm ? nm : "<unnamed>", (unsigned)ps.overflow_win,
                ps.overflow_at_vert, GUI_MAX_VERTS, ps.overflow_at_idx, GUI_MAX_IDX,
                ps.overflow_at_cmd, GUI_MAX_CMDS );
        fflush( stdout );   /* flush the diagnostic before the once-assert below can trap */
    }
    if ( s_tess.overflow )
        s_tess_stats.overflow_ever = true;

    /* Break once (debug) so you can catch which frame / UI blew the caps under the debugger; the
       macro self-latches, so a persistent overflow does not re-trap every frame.  Non-fatal: skip
       past it (or a release build) and the app keeps running with the dropped geometry. */
    ORB_ASSERT_MSG_ONCE( !s_tess.overflow, "gui draw list overflow -- geometry dropped; raise "
                                           "GUI_MAX_VERTS / GUI_MAX_IDX / GUI_MAX_CMDS (gui.h)" );

    /* Pipeline-dashboard snapshot: everything above is final for the frame -- slots placed,
       dispatch z-sorted, stats accumulated.  A no-op unless GUI_PIPELINE_DASHBOARD. */
    DASH_CAPTURE_BUILD();
}

// clang-format on
/*============================================================================================*/
