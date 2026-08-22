/*==============================================================================================

    /gui/render/pipeline/gui_build_diff.c -- Change detection (BUILD step 1).

    Split out of gui_build_cache.c: this file is the first of the two BUILD-phase workers that
    file's cache_build_frame drives (see that file's header for the full three-phase pipeline
    picture).  This one answers "did anything change": it hashes each window's commands, diffs
    against last frame, and produces s_cache -- the per-window changed/unchanged table that
    gui_build_place.c's placement pass reads to decide reuse vs retessellate.

    Each frame accumulates every window's per-command hashes (baked at emit by draw_hash_cmd)
    into one per-window hash, sorts by window id, then compares against last frame's sorted table
    in one linear scan.  A window whose hash matches is unchanged and may reuse its geometry.
    any_changed is the coarse signal that at least one window appeared, vanished, or changed.

    Hashing goes deep for TEXT and POLYLINE commands (which point into side pools): a
    same-length edit leaves the command struct byte-identical, so pool bytes are folded in too.

==============================================================================================*/
// clang-format off

/* Per-window diff record.  cur[] is rebuilt each frame; prev[] is last frame's snapshot.
   z/vp are accumulated (max-z, last-vp across segments) so the slot builder needs no rescan. */

typedef struct
{
    gui_id_t win;            // window whose commands this record hashes; 0 = empty record, never used
    u32      hash;           // accumulated hash of all commands in this window (via draw_hash_cmd)
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

/* Per-window segment chain links, parallel to s_draw.segs (GUI_MAX_SEGS fits u16).  
   Rebuilt each frame by cache_diff_windows; only meaningful through the records' 
   seg_head/seg_tail. */

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

/*============================================================================================*/

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
build_any_changed( void )
{
    return s_cache.any_changed;
}

/* This frame's window count (forward-declared in gui_build_cache.c): cache_build_frame folds it
   into win_total / win_hwm / the overflow report, all after cache_diff_windows has run. */

static u32
cache_cur_win_count( void )
{
    return s_cache.cur_n;
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
    u32 memo_bi   = ~0u;                            /* last record hit -- consecutive segments usually share a window */
    u8  clip_used[ GUI_MAX_CLIP_RECTS ] = { 0 };    /* clip table entries a band-0 command references */

    for ( u32 si = 0; si < nseg; ++si )
    {
        if ( segs[ si ].lo == segs[ si ].hi ) continue;   // empty span

        gui_id_t win = segs[ si ].win;

        if ( segs[ si ].band == 0 )   /* debug-band UI never counts in the stats it displays */
            total_cmd += segs[ si ].hi - segs[ si ].lo;

        /* Find or create the per-window record.  The memo short-circuits the scan for the common
           run of consecutive same-window segments (a window's z / viewport / band changes). */
        u32 bi;
        if ( memo_bi < s_cache.cur_n && s_cache.cur[ memo_bi ].win == win )
            bi = memo_bi;
        else
            for ( bi = 0; bi < s_cache.cur_n; ++bi )
                if ( s_cache.cur[ bi ].win == win ) break;

        if ( bi == s_cache.cur_n )
        {
            /* A window past RENDER_MAX_WIN gets no record, no slot, and never tessellates -- it is
               DROPPED from rendering this frame, not just uncached.  Latched into the same wall
               mask the arenas use so the end-of-build report names it; warned here as well because
               this happens before any of them, while the culprit window is still in hand. */
            if ( s_cache.cur_n >= RENDER_MAX_WIN )
            {
                s_diff_window_overflow |= TESS_OVF_WINDOWS;
                GUI_WARN_ONCE( "more than %u windows this frame -- extra windows "
                               "are not rendered. Raise RENDER_MAX_WIN (gui_render.h).\n",
                               RENDER_MAX_WIN );
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

        /* Fold z, vp and band into the hash.  The FONT ID is not folded here because it rides
           each text command and is folded by draw_hash_cmd, whose result this loop already
           accumulates below, so a font change still dirties its window.
           NO blanket atlas-generation fold: text addresses glyphs by stable table id (the glyph
           table rewrites in place on a repack), fills are GUI_OP_SELF, and emit-baked uvs
           (icons, images) re-resolve on the dirty frame the atlas upload itself raises -- so a
           coverage or SDF repack invalidates NOTHING here, which is the repack-free prize the
           glyph table exists for.  The two tess-time-resolved uv consumers left are folded per
           COMMAND in the loop below (sprites, dash rows).

           One text case is NOT repack-proof: a glyph straddling its run's window edge carries a
           narrowed uv span, which is per-instance and has no table entry, so it bakes a rect like
           any other textured quad (gui_build_tess_text.c, tess_text_n).  At most the two end glyphs of
           a cut run, and a repack leaves them sampling the wrong pixels until that window changes
           for another reason.  Folding the generation for every TEXT command would cost the prize
           above to fix a glyph-wide artefact on a boot-time event. */

        u32 h = s_cache.cur[ bi ].hash;
        h = fnv1a_u32( h, segs[ si ].z    );
        h = fnv1a_u32( h, segs[ si ].vp   );
        h = fnv1a_u32( h, segs[ si ].band );   /* band flip must re-tessellate (slot changes ends) */

        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            /* Clip usage is marked before the volatile skip below -- a volatile command still
               draws under its clip, it only stays out of the window hash. */
            if ( segs[ si ].band == 0 )
                 clip_used[ s_draw.cmds[ i ].clip_idx ] = 1;

            /* A volatile-tagged command NEVER participates in its window's hash -- the block is
               presentation-only by contract and patched out of band (volatile_update on idle
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

            /* Per-command generation folds: the commands whose uvs are still resolved at TESS time
               against a movable atlas placement.  A sprite bakes its atlas rect fresh each
               tessellation; a dashed line bakes the assist row's V, which moves when the coverage
               atlas grows; an fx_box CARRYING A SHAPE resolves the SDF tenant it names the same
               way.  Folding only on the commands that carry the dependency keeps every other
               window repack-immune -- which is why the shape case tests the lane rather than the
               type: an ordinary rounded box states no atlas and must stay immune. */

                 if ( s_draw.cmds[ i ].type == GUI_CMD_SPRITE )      h = fnv1a_u32( h, res_sprite_generation() );
            else if ( s_draw.cmds[ i ].type == GUI_CMD_DASHED_LINE ) h = fnv1a_u32( h, res_atlas_generation() );
            else if ( s_draw.cmds[ i ].type == GUI_CMD_FX_BOX
                   && draw_cmd_ext_slot( s_draw.cmds[ i ].offset )->fx_box.shape != GUI_SHAPE_NONE )
                                                                     h = fnv1a_u32( h, res_sdf_generation() );
        }
        s_cache.cur[ bi ].hash = h;

        if ( segs[ si ].z > s_cache.cur[ bi ].z ) 
             s_cache.cur[ bi ].z = segs[ si ].z;

        s_cache.cur[ bi ].vp = segs[ si ].vp;

        if ( segs[ si ].band != 0 ) 
             s_cache.cur[ bi ].band = 1;   /* sticky: any debug seg tags the window */
    }

    /* Sort cur[] band-major, win-minor (cache_rec_before) -- debug-band windows pack after every
       main-band window.  Insertion sort over RENDER_MAX_WIN = 32 elements: O(n) when the window
       set is stable (the common case, already sorted from last frame), O(n^2) at worst.
       prev[] is kept in the same order via the memcpy below, so the diff is a single linear scan. */

    for ( u32 a = 1; a < s_cache.cur_n; ++a )
    {
        render_win_hash_t key = s_cache.cur[ a ]; u32 b = a;
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
           real frame it is visible -- if that alone kept any_changed true, build_any_changed()
           would report "something changed" forever and frame_dirty would never go false again,
           silently defeating idle-skip for the WHOLE app for as long as the readout is on screen.
           s_cache.cur[i].changed above still flags true so cache_build_frame retessellates the
           readout's own slot (its digits really did change); the GLOBAL any_changed signal
           ignores debug-band HASH changes.  (A debug window APPEARING or VANISHING still raises
           it through the count seed above -- a set change needs a running frame anyway.) */
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

    s_stats.accum.cmd_count      = total_cmd;               /* band-0 only: application cost      */
    s_stats.accum.cmd_count_all  = s_draw.cmd_count;        /* physical pool fill, both bands     */
    s_stats.accum.seg_count      = nseg;
    s_stats.accum.text_pool_used = s_draw.text_pool_used;   /* frame emit is complete at the build seam */

    /* Distinct clip rects the main band drew under this frame.  Counted by command reference, not
       raw table size (s_draw.clip_table_n): the raw table also holds debug-band pushes and pushes
       no surviving command references, so it overstates what the application itself costs.  The
       physical table fill is published alongside (clip_count_all) for the capacity view. */

    u32 total_clip = 0;
    for ( u32 ci = 0; ci < GUI_MAX_CLIP_RECTS; ++ci )
        total_clip += clip_used[ ci ];

    s_stats.accum.clip_count     = total_clip;
    s_stats.accum.clip_count_all = s_draw.clip_table_n;
}

// clang-format on
/*============================================================================================*/
