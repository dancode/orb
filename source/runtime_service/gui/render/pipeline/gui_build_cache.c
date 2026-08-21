/*==============================================================================================
    gui/render/pipeline/gui_build_cache.c -- Retained frame-geometry cache (BUILD phase).

    The render pipeline has three phases.  This file is the middle one, and is itself split
    across three units so no single file carries the whole BUILD phase:

        EMIT   (gui_emit_*.c)      widgets push semantic shapes -> s_draw command list,
                                    cut into per-(win,z,vp,band) segments, one hash baked per command.
        BUILD  (this file +        once per frame (cache_build_frame, this file): diff each
               gui_build_diff.c +  window's commands against last frame (gui_build_diff.c),
               gui_build_place.c) reuse unchanged geometry in place or tessellate changed windows
                                    (gui_build_place.c), then z-sort the result into a dispatch
                                    table.  This file owns the shared slot/stats state both workers
                                    read and write, plus the top-level driver that sequences them.
        RENDER (gui_render_submit.c) once per surface: upload the surface's slot-union span (each
                                    frame-in-flight region must hold complete geometry, so the
                                    cache saves TESSELLATION, not upload) and emit one draw
                                    call per cached GPU command.

    BUILD runs lazily on the first surface flush (cache_build_frame, guarded by s_frame_built)
    because the semantic command list is shared across every surface -- the geometry it produces
    is surface-independent.  build_frame_reset clears the guard at frame_begin.

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

/* Walls this build hit OUTSIDE the tessellation passes (TESS_OVF_*, gui_build_tess_state.c) -- today
   just the window cap, latched during the diff.  Kept apart from s_tess.overflow because that one
   is a property of ONE placement pass and tess_reset clears it, which the repack retry relies on;
   this is a property of the whole build.  Cleared at the top of cache_build_frame, folded into the
   overflow report at the bottom. */
static u32 s_build_walls;

void
build_frame_reset( void )
{
    s_frame_built = false;
}

/*==============================================================================================
    Per-frame render stats.

    accum is built by two phases that do NOT run on the same schedule: BUILD (cache_diff_windows /
    cache_build_frame -- cmd_count, quad_count, prim_count, win_total, win_retained,
    quad_retained) runs at most once per REAL frame, guarded by s_frame_built, and is skipped
    entirely on an idle frame (frame_dirty()==false); SUBMIT (cache_count_draw_calls /
    cache_count_upload -- draw_calls, upload_batches, upload_bytes) runs every single frame, real
    or idle, once per surface flush, because the GPU replays cached geometry every frame regardless.

    build_stats_publish runs at every frame_begin, real or idle.  It must NOT blanket-zero
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

/*==============================================================================================
    Phase timing.

    The render server links no clock, so the host's arrives through the same one-way seam
    gui_render_set_time uses.  Without one every zone reads zero and the arithmetic below is two
    predictable branches -- the reason this is not compiled out: the phase split is the only way to
    tell a frame that tessellated too much from one that uploaded too much, and a number that
    exists only in a debug build is a number nobody has when it matters.
==============================================================================================*/

static gui_clock_fn s_zone_clock;

void
gui_render_set_clock( gui_clock_fn fn )
{
    s_zone_clock = fn;
}

/* Open a zone: the clock now, or 0 when there is none to read. */
static f64
zone_begin( void )
{
    return s_zone_clock ? s_zone_clock() : 0.0;
}

/* Close a zone into `dst` (ms).  Adds rather than assigns so a phase that runs more than once in a
   frame -- cache_place_slots under the repack retry, gui_render_flush per surface -- reports what
   the frame actually spent, not what its last pass did.  Callers zero `dst` where the phase's
   publish rule says to. */
static void
zone_end( f32* dst, f64 t0 )
{
    if ( s_zone_clock )
        *dst += (f32)( ( s_zone_clock() - t0 ) * 1000.0 );
}

void
build_stats_publish( void )
{
    s_stats.published = s_stats.accum;
    s_stats.accum.draw_calls       = 0;
    s_stats.accum.upload_batches   = 0;
    s_stats.accum.upload_bytes     = 0;
    s_stats.accum.volatile_patched = 0;   // per-frame event count, same reset rule as draw_calls
    s_stats.accum.submit_ms        = 0.0f; // summed per surface flush, so same reset as draw_calls
    s_stats.accum.gpu_ms           = 0.0f; // same summing rule; each surface adds its context's sample
}

/* Defined here (forward-declared in gui_build_volatile.c, included just above this file) because
   it needs s_stats.  Counts a volatile row patched in place this frame, whether via idle replay
   (volatile_update) or a live real-frame reuse-patch (volatile_patch_reused_window). */
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

void build_set_retained_skip( bool on ) { s_retained_cache = on; }
bool build_retained_skip    ( void )    { return s_retained_cache; }

/* Debug-band windows (GUI_WIN_DEBUG_BAND: the perf overlay, the pipeline dashboard, their
   popups/tooltips) are exempted from: (1) the quad/window totals they may themselves display
   (gui_render_stats_t), and (2) contributing their ever-changing hashes to any_changed / the
   frame_dirty signal that drives idle-skip.  Without (2), simply having a live readout visible
   would keep the whole app rebuilding every frame regardless of anything else being idle.
   Debug-band windows still hash, diff, and retessellate normally -- they render; only the
   metrics ignore them, and the slot packer places them after every main-band slot (see the
   band-major sort in cache_diff_windows). */

/*==============================================================================================
    Window geometry slots -- the retained geometry store.

    Each window owns one slot that caches its tessellated geometry: a quad span, a style-record
    span, and the GPU draw commands that replay them.  An unchanged window replays its commands
    from the stable cache instead of re-tessellating; a changed window tessellates into its slot.
    Slots are z-sorted into the dispatch table that SUBMIT walks.

    Slot tables ping-pong between two backing stores (s_slots_a / s_slots_b) so this frame can
    read last frame's geometry positions (s_slots_prev) while building this frame's (s_slots).

    No separate prev-geometry buffer: s_tess.quads/prims persist between frames, so each
    window's geometry remains at its prev->quad_base until overwritten.  The reuse path advances
    the write head by quad_alloc (not quad_count) to keep the SLOT_QUAD_PAD gap intact, which
    absorbs minor in-place growth without touching adjacent slots.
==============================================================================================*/

/* RENDER_MAX_WIN / SLOT_QUAD_PAD live in gui_render.h (the dashboard snapshot types are sized
   by them). */

/* Max GPU draw commands cached per slot; most windows have 2-4, but every volatile block adds
   its own commands + reserved dormant pads (unmergeable across the reservation seams), so a
   window dense with volatile widgets multiplies fast.  A window that exceeds the cap is NOT
   truncated -- it goes uncacheable (see cache_slot_tessellate) and re-tessellates every real
   frame; the cap trades stable-cache memory against how large a window can be and still be
   retained. */

#define WIN_SLOT_CMD_MAX  16

/* Proactive compaction threshold.  The reuse allocator is bump-only: new windows always tessellate
   at the tail (past every live reservation), so a closed/relocated window's space becomes a hole
   nothing refills -- a churny UI (dragging across a menu bar, each submenu a fresh window that
   leaves a hole when it closes) marches the tail up and strands dead space behind it.  Waiting for
   the arena to hit the cap before repacking means the fill only ever grows during churn.  So once
   the DEAD (unreserved) space in the used range [0, tail) reaches this percentage, cache_build_frame
   repacks proactively -- the backend self-compacts on a cheap frame instead of on a user event or a
   hard overflow.  Padding (quad_alloc - quad_count) is NOT counted as dead, so a freshly repacked
   arena reads ~0% and the trigger cannot thrash frame-to-frame. */
#define GUI_REPACK_FRAG_PCT   25
#define GUI_REPACK_FRAG_FLOOR 2048   /* skip the check below this many dead quads -- noise */

/* One cached GPU draw command.  Packed AOS so replaying a slot's commands touches one region.
   z is per-slot (the window's max segment z), not per-command; lqbase is slot-local so the
   reuse path needs no fixup when the slot's absolute quad_base is unchanged. */
typedef struct
{
    gui_gpu_cmd_t cmd;     // texture slot, element count (quads)
    i16           vp;      // viewport this command targets (GUI_VP_INVALID = dormant volatile pad)
    u16           lqbase;  // quad base relative to slot->quad_base (0-relative)

} win_slot_cmd_t;

/* One window's cached geometry position and the command range that replays it. */
typedef struct
{
    gui_id_t win;
    u32      z;
    u32      quad_base, quad_count, quad_alloc;  // quads: absolute position, actual count, padded reservation
    u32      prim_base, prim_count, prim_alloc;  // style records: same three, in their own arena
    u32      cmd_base,  cmd_count;               // range into s_tess.gpu_cmds[] for this window
    u32      tess_gen;                           // generation of the tess pass that produced the geometry
    u32      text_quads, text_runs;              // of quad_count, the glyph share and the runs it came
                                                 //   from -- taken at tessellation and carried across
                                                 //   reuse frames, since retained geometry is never
                                                 //   re-walked (gui_build_tess_state.c, slot_text_*)
    u8       vp;                                 // viewport (GUI_MAX_VIEWPORTS = 4)
    u8       band;                               // arena band (0 = main UI, 1 = debug/diagnostic)
    bool     valid;                              // true once geometry has been tessellated at least once
    bool     cmd_cached;                         // command run fit the stable cache; false = the window
                                                 //   overflowed WIN_SLOT_CMD_MAX and must re-tessellate
                                                 //   every real frame instead of reusing (never truncate)

    /* The slot's LOCAL clip table: the rects this window's quads name through their clip field,
       clip band (gui_render.h, gui_clip_entry_t).  Written by tess_clip_local while the window
       tessellates, carried verbatim across reuse frames, and uploaded by the flush into the
       window's FIXED SLAB of the frame clip region (cache_idx * GUI_WIN_CLIP_MAX) -- only when
       s_clip_slab_pending says the GPU copy is stale.  Lives on the slot (not the stable command
       cache) because every path that renders the window needs it -- including uncacheable
       command runs. */
    u32              cache_idx;   // the window's id-keyed stable cache entry == its clip slab key
    gui_clip_entry_t clips[ GUI_WIN_CLIP_MAX ];
    u32              clip_count;

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

/* Clip-slab upload masks, keyed like the cache above: bit r set = region r's GPU copy of this
   window's clip slab is stale.  tess_clip_local sets all bits when it appends an entry; the
   flush clears one bit per slab it uploads.  One bit per (frame-in-flight, viewport) region. */

ORB_STATIC_ASSERT( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS <= 8,
                   "clip slab pending mask is a u8 -- one bit per (frame, viewport) region" );

static u8               s_clip_slab_pending[ RENDER_MAX_WIN ];

/* Record-range upload masks, keyed and shaped exactly like the clip masks above, and needed for a
   reason the clip slabs do not have: a clip slab sits at a FIXED offset (cache_idx * the per-window
   cap), so only its content can go stale, while a record range is packed and therefore moves.  So
   this is set by every fresh tessellation -- which is the only thing that can change either the
   content or the base -- and cleared one bit per range the flush uploads. */

static u8               s_prim_range_pending[ RENDER_MAX_WIN ];

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

static u32              s_slot_count, s_slot_prev_count;
static win_geo_slot_t   s_slots_a  [ RENDER_MAX_WIN ];
static win_geo_slot_t   s_slots_b  [ RENDER_MAX_WIN ];
static win_geo_slot_t*  s_slots      = s_slots_a;   // current frame (write)
static win_geo_slot_t*  s_slots_prev = s_slots_b;   // previous frame (read)
static win_geo_slot_t*  s_dispatch [ RENDER_MAX_WIN ];
static u32              s_dispatch_count;

/* Volatile widgets (volatile_cb_open/_stamp/_close, volatile_update, the registry and
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
cache_slot_lookup( gui_id_t win, u32* quad_base, u32* prim_base, u32* cmd_base,
                   u32* tess_gen )
{
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        if ( s_slots[ i ].win != win || !s_slots[ i ].valid )
            continue;
        *quad_base = s_slots[ i ].quad_base;
        *prim_base = s_slots[ i ].prim_base;
        *cmd_base  = s_slots[ i ].cmd_base;
        *tess_gen  = s_slots[ i ].tess_gen;
        return true;
    }
    return false;
}

/* Mark a window's record range stale in every upload region (forward-declared in
   gui_build_volatile.c).  A volatile patch rewrites records in place inside the slot's range, and
   records upload per SLOT rather than per byte span -- so unlike the vertex and index bytes there
   is no dirty span to union, just the slot's own pending mask. */
static void
volatile_prim_range_dirty( gui_id_t win )
{
    for ( u32 i = 0; i < s_slot_count; ++i )
        if ( s_slots[ i ].win == win && s_slots[ i ].valid )
        {
            s_prim_range_pending[ s_slots[ i ].cache_idx ] = 0xFF;
            return;
        }
}

/* A window's CURRENT viewport + arena band by id (forward-declared in gui_build_volatile.c): a
   volatile patch tags its dirty spans with the viewport whose flush must re-upload them and the
   band whose stats they belong to.  Same s_slots view as cache_slot_lookup; GUI_VP_INVALID when
   the window has no live slot. */
static u8
cache_slot_vp( gui_id_t win, u8* out_band )
{
    for ( u32 i = 0; i < s_slot_count; ++i )
        if ( s_slots[ i ].win == win && s_slots[ i ].valid )
        {
            *out_band = s_slots[ i ].band;
            return s_slots[ i ].vp;
        }
    *out_band = 0;
    return (u8)GUI_VP_INVALID;
}

/* Point s_tess at the window slot's LOCAL clip table (forward-declared in gui_build_volatile.c):
   a volatile patch's scratch tessellation resolves its clip-band indices against the table the
   capture built, so an unchanged clip patches to the same band bits.  Same s_slots view as
   cache_slot_lookup, and callers already gate on it succeeding. */
static bool
cache_slot_clips_bind( gui_id_t win )
{
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        if ( s_slots[ i ].win != win || !s_slots[ i ].valid )
            continue;
        s_tess.slot_clips        = s_slots[ i ].clips;
        s_tess.slot_clip_count   = &s_slots[ i ].clip_count;
        s_tess.slot_clip_pending = &s_clip_slab_pending[ s_slots[ i ].cache_idx ];
        s_tess.clip_memo_ci      = 0xFF;
        return true;
    }
    return false;
}

/* Far edge of all LIVE geometry -- the floor past which s_tess is free scratch space.  Forward-
   declared in gui_build_volatile.c: a volatile patch tessellates into scratch at s_tess.quad_count
   and must assert it is at/beyond this, or the scratch scribbles through a live slot's geometry
   (the tooltip-vs-pulse collision, fixed 2026-07-04).

   Spans BOTH slot tables: mid-build-loop, s_slots holds only the windows placed so far, while a
   reused window not yet reprocessed still has its geometry sitting at LAST frame's position
   (s_slots_prev) -- exactly where the buggy inline patch scribbled over the tooltip.  Taking the
   max over both makes the guard see that still-live geometry the current table hasn't caught up to
   yet.  On idle frames both tables are last frame's, so the max is simply the true tail.

   !RELEASE only, matching the sole call site (that assert). */
#if !RELEASE
static void
cache_slots_extent( u32* out_quad_end, u32* out_prim_end )
{
    u32 ve = 0, pe = 0;
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        if ( !s_slots[ i ].valid )
            continue;
        u32 v = s_slots[ i ].quad_base + s_slots[ i ].quad_alloc;
        u32 p = s_slots[ i ].prim_base + s_slots[ i ].prim_alloc;
        if ( v > ve ) ve = v;
        if ( p > pe ) pe = p;
    }
    for ( u32 i = 0; i < s_slot_prev_count; ++i )
    {
        if ( !s_slots_prev[ i ].valid )
            continue;
        u32 v = s_slots_prev[ i ].quad_base + s_slots_prev[ i ].quad_alloc;
        u32 p = s_slots_prev[ i ].prim_base + s_slots_prev[ i ].prim_alloc;
        if ( v > ve ) ve = v;
        if ( p > pe ) pe = p;
    }
    *out_quad_end = ve;
    *out_prim_end = pe;
}
#endif

/* cache_invalidate_window lives below, after s_cache is declared. */

/*==============================================================================================
    Forward declarations for the two BUILD-phase workers.

    cache_diff_windows lives in gui_build_diff.c (change detection, fills s_cache); it also
    defines render_win_hash_t, which gui_build_place.c's cache_tess_window walks.
    cache_place_slots lives in gui_build_place.c (per-window reuse/tessellate + dispatch fill);
    it and cache_build_frame below share cache_place_stats_t, defined here since cache_build_frame
    holds one on its stack across both placement passes.
==============================================================================================*/

static void cache_diff_windows( void );
static u32  cache_cur_win_count( void );

/* Debug-build slot-layout guard, defined alongside cache_dump_slots in gui_build_place.c;
   cache_build_frame runs it once every frame right after placement. */
#if !RELEASE
static void cache_validate_geometry( void );
#endif

/* Accumulators one placement pass produces for the stats/report code after it. */
typedef struct
{
    u32      quad_retained, win_retained;
    u32      total_quad, total_prim, overlay_win;
    u32      reserved_quad;                 /* sum of quad_alloc over ALL placed slots */
    gui_id_t overflow_win;                  /* first window a pool spilled under (NONE = outside the loop) */
    u32      overflow_at_quad, overflow_at_cmd, overflow_at_prim;
    gui_id_t reused_volatile_wins[ RENDER_MAX_WIN ];
    u32      reused_volatile_n;

} cache_place_stats_t;

static void cache_place_slots( bool allow_reuse, cache_place_stats_t* st );

/*==============================================================================================

    cache_build_frame (BUILD step 2) -- diff, reuse or re-tessellate per window, z-sort.

    Runs once per frame (guarded by s_frame_built).  Produces the geometry (s_tess.quads/prims),
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

static void
cache_build_frame( void )
{
    if ( s_frame_built )
         return;

    s_frame_built = true;
    s_build_walls = 0u;

    /* Before anything tessellates: refresh the glyph UV table if a font's tenant changed
       or an atlas repacked, so every ID emitted this frame and every rect behind it come
       from one build. */

    glyph_table_sync();

    /* Close the still-open final segment so diff and tess see its full [lo, hi) range. */

    if ( s_draw.seg_count > 0 )
         s_draw.segs[ s_draw.seg_count - 1 ].hi = (u16)s_draw.cmd_count;

    /* Command-stepper capture: the frame's segments are closed and every emit pool is
       complete, nothing is diffed or tessellated yet -- the exact seam to freeze the live
       command list. 
       
       A no-op unless GUI_CMD_STEPPER and a capture was requested (step_capture). */

    STEP_CAPTURE_BUILD();

    /* Text-drag-selection run capture: same seam.  Rebuilds the selection run buffer for any window
       marked GUI_WIN_TEXT_SELECT this frame; a two-branch no-op while no flagged window is
       live. See render/gui_select_capture.c. */

    select_capture_build();

    /* Both BUILD zones are assignments per real frame, so they zero here rather than in
       build_stats_publish: an idle frame never reaches this function and must keep reporting what
       the geometry on screen actually cost. */

    s_stats.accum.diff_ms = 0.0f;
    s_stats.accum.tess_ms = 0.0f;

    /* Step 1: hash-diff all windows, fill s_cache, accumulate cmd_count stats. */

    f64 t_diff = zone_begin();
    cache_diff_windows();
    zone_end( &s_stats.accum.diff_ms, t_diff );

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
    f64                 t_tess = zone_begin();
    cache_place_slots( true, &ps );
    zone_end( &s_stats.accum.tess_ms, t_tess );

    /* Repack triggers, both handled by a single from-scratch re-place (reuse off, pack from 0):
         1. Overflow -- the reuse path spilled the arena.  Often pure fragmentation (holes from
            vanished / relocated windows), so a compacting retry usually fits; an overflow that
            SURVIVES the repack means the content genuinely exceeds the arena.
         2. Proactive (GUI_REPACK_FRAG_PCT) -- the bump allocator never refills holes, so dead space
            only grows during churn.  Measure it (tail minus what live slots actually reserve) and
            compact once it crosses the threshold, so the backend self-heals on a cheap frame
            instead of waiting for a user event or a hard overflow.  Floored so a near-empty arena
            never trips on a high percentage of a tiny number. */
    u32  dead_v = s_tess.quad_count > ps.reserved_quad ? s_tess.quad_count - ps.reserved_quad : 0;
    bool frag   = ( dead_v >= GUI_REPACK_FRAG_FLOOR
                    && dead_v * 100u >= s_tess.quad_count * GUI_REPACK_FRAG_PCT );

    if ( s_tess.overflow || frag )
    {
        /* Every slot relocates: fine dirty spans cannot describe this, so the geometry
           generation bumps and every in-flight upload region goes stale (full re-upload). */
        ++s_geo_gen;
        t_tess = zone_begin();
        cache_place_slots( false, &ps );
        zone_end( &s_stats.accum.tess_ms, t_tess );   /* the retry is real cost this frame paid */
    }

    /* Deferred volatile patches for reused windows: every slot is now placed and s_tess.quad_count
       is the true tail, so volatile_patch's scratch tessellation lands past all live geometry
       instead of over a later slot's quads (see reused_volatile_wins above). */
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
    s_stats.accum.quad_count    = ps.total_quad;
    s_stats.accum.prim_count    = ps.total_prim;
    /* Physical arena fills: what the caps are actually hit against.  The write head, not the sum of
       the slots -- it carries every slot's reservation padding and both bands. */
    s_stats.accum.quad_count_all = s_tess.quad_count;
    s_stats.accum.prim_count_all = s_tess.prim_count;
    s_stats.accum.win_total     = cache_cur_win_count() - ps.overlay_win;
    s_stats.accum.win_retained  = ps.win_retained;
    s_stats.accum.quad_retained = ps.quad_retained;

    /* Track pool high-water marks.  The total (both bands) and the main band alone peak
       independently, so each gets its own accumulator. */
    if ( s_tess.quad_count           > s_tess_stats.quad_hwm       ) s_tess_stats.quad_hwm       = s_tess.quad_count;
    if ( s_tess.prim_count           > s_tess_stats.prim_hwm       ) s_tess_stats.prim_hwm       = s_tess.prim_count;
    if ( s_tess.cmd_count            > s_tess_stats.cmd_hwm        ) s_tess_stats.cmd_hwm        = s_tess.cmd_count;
    if ( cache_cur_win_count()       > s_tess_stats.win_hwm        ) s_tess_stats.win_hwm        = cache_cur_win_count();
    if ( s_tess_stats.band0_quad_end > s_tess_stats.band0_quad_hwm ) s_tess_stats.band0_quad_hwm = s_tess_stats.band0_quad_end;

    /* Single overflow catch for the whole build.  The reservation sites only latch their wall into
       s_tess.overflow and drop the primitive (non-fatal -- the frame still submits everything that
       fit), so the report happens ONCE here, with the frame fully tessellated: which caps were hit,
       what each pool filled to, and the window that hit the first of them.  Naming the culprit is
       what turns "something overflowed" into a fixable bug report, since the classic symptom -- a
       window's late-tessellated chrome silently vanishing -- points nowhere on its own.  The log
       line and the dashboard's OVERFLOWED marker both persist past the assert below. */
    /* A spill outside the per-window loop (a deferred volatile patch) captured no culprit window,
       so report the final pool fills rather than a stale zero. */
    if ( s_tess.overflow && ps.overflow_win == GUI_ID_NONE )
    {
        ps.overflow_at_quad = s_tess.quad_count;
        ps.overflow_at_cmd  = s_tess.cmd_count;
        ps.overflow_at_prim = s_tess.prim_count;
    }

    u32  hit = s_tess.overflow | s_build_walls;   /* this pass's walls, plus the diff's */
    char walls[ 128 ];
    tess_overflow_walls( hit, walls, (u32)sizeof( walls ) );

    if ( hit & ~s_tess_stats.overflow_walls )
    {
        /* Re-reported whenever a NEW wall appears, not only on the first spill ever: hitting the
           quad cap and later hitting the style cap are different bugs with different fixes. */
        const char* nm = ( ps.overflow_win != GUI_ID_NONE ) ? gui_debug_name( ps.overflow_win ) : NULL;
        
        /* The default sink flushes, so this lands before the once-assert below can trap. */
        gui_log( GUI_LOG_WARN, "gui BUILD overflow: (%s) -- content dropped tessellating window '%s' (id 0x%08X);",                 
                 walls, nm ? nm : "<unnamed>", (unsigned)ps.overflow_win );
        gui_log( GUI_LOG_WARN,"gui BUILD stats: pools at %u/%u quads, %u/%u styles, %u/%u gpu cmds, %u/%u windows",
                 ps.overflow_at_quad, GUI_MAX_QUADS,
                 ps.overflow_at_prim, GUI_MAX_PRIMS,
                 ps.overflow_at_cmd,  GUI_MAX_CMDS,
                 cache_cur_win_count(), RENDER_MAX_WIN );
    }
    s_tess_stats.overflow_walls |= hit;

    /* Break once (debug) so the frame that blew a cap can be caught under the debugger; the macro
       self-latches, so a persistent overflow does not re-trap every frame.  Non-fatal: skip past it
       (or a release build) and the app keeps running with the dropped content -- the log line above
       carries the cap names and the fills, which is what actually diagnoses it. */
    ORB_ASSERT_MSG_ONCE( !hit,
                         "gui build overflow -- content dropped; see the preceding gui log line "
                         "for which cap was hit and what each pool filled to" );

    /* Pipeline-dashboard snapshot: everything above is final for the frame -- slots placed,
       dispatch z-sorted, stats accumulated.  A no-op unless GUI_PIPELINE_DASHBOARD. */
    DASH_CAPTURE_BUILD();
}

// clang-format on
/*============================================================================================*/
