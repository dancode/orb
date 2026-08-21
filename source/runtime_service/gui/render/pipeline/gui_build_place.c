/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_place.c -- Per-window placement (BUILD step 2).

    Split out of gui_build_cache.c: this file is the second of the two BUILD-phase workers that
    file's cache_build_frame drives (see that file's header for the full three-phase pipeline
    picture).  Given gui_build_diff.c's per-window changed/unchanged table, this file decides,
    window by window, whether to replay cached geometry in place or retessellate, and runs the
    whole-frame placement pass (cache_place_slots) that cache_build_frame calls once for the
    normal path and, on overflow or fragmentation, a second time as a compacting repack.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Per-window tessellation (BUILD step 2 helper).

    Gathers a window's commands (via the segment chain cache_diff_windows built onto its diff
    record) into an emission-order permutation and hands it to tess_dispatch.  Emission order IS
    paint order, so overpaint relationships (titlebar chrome over scrolled content) hold without
    any per-clip care -- and since the clip rides the vertex (gui.h, the clip band), equal-clip
    commands never need to be adjacent to share a draw call.

    The one reorder left: a CONFINED volatile range emits at the TAIL, after every plain command.
    A block owns its GPU command(s) either way (a patch must be able to rewrite their elem_counts),
    so mid-body it cuts the window into three draw calls; at the tail the body on both sides of it
    merges back into one, and its reservation padding sits at the slot's end.  Painting it last is
    safe precisely because it is confined -- volatile_cb_close scissored the range to its own
    layout cell, so it cannot overpaint chrome or a sibling no matter where in the order it lands.
    An UNCONFINED range (its callback never opened the footprint probe) keeps its emission
    position: nothing bounds where it paints.

    Only commands whose clip is EMPTY are dropped: they can paint nothing (a fully scrolled-out
    child, a hidden volatile range), and dropping them here is what keeps them out of the
    geometry buffers entirely.  Volatile ranges stay contiguous in emission order minus those
    holes, which is what tess_dispatch's range tracking brackets -- the tail pass appends whole
    ranges in emission order, so contiguity survives the hoist.

    z is NOT sorted here -- a window occupies one slot whose dispatch z is its max segment z,
    keeping all of a window's geometry contiguous; cache_build_frame z-sorts the slots after.
==============================================================================================*/

/* Permutation output scratch, reused by every cache_tess_window call (cache_build_frame is
   single-threaded and guarded against re-entry).  u16: order values are command indices
   (< GUI_MAX_CMDS, asserted u16-safe at gui_cmd_seg_t). */
static u16 s_win_order[ GUI_MAX_CMDS ];

static void
cache_tess_window( const render_win_hash_t* wh )
{
    const gui_cmd_seg_t* segs = s_draw.segs;

    /* Pass 1: plain commands (and any unconfined volatile range) in emission order. */
    u32  n       = 0;
    bool any_vol = false;
    for ( u16 si = wh->seg_head; si != SEG_CHAIN_END; si = s_seg_next[ si ] )
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            if ( rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
                continue;
            gui_id_t vid = s_draw.cmd_volatile_id[ i ];
            if ( vid != GUI_ID_NONE && volatile_row_confined( vid ) )
            {
                any_vol = true;
                continue;
            }
            s_win_order[ n++ ] = (u16)i;
        }

    /* Pass 2: confined volatile ranges, whole and in emission order, at the tail. */
    if ( any_vol )
        for ( u16 si = wh->seg_head; si != SEG_CHAIN_END; si = s_seg_next[ si ] )
            for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
            {
                if ( rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
                    continue;
                gui_id_t vid = s_draw.cmd_volatile_id[ i ];
                if ( vid != GUI_ID_NONE && volatile_row_confined( vid ) )
                    s_win_order[ n++ ] = (u16)i;
            }

    tess_dispatch( s_draw.cmds, s_win_order, n, wh->win );
}

/*==============================================================================================
    Debug diagnostics -- dump the cached-geometry slot table, and a per-frame layout guard.

    cache_dump_slots prints every slot's window, z/vp, and its quad / command bounds.  Exposed to
    hosts via gui()->debug_dump_geometry() for on-demand inspection, and printed automatically by
    cache_validate_geometry right before it trips an assert.

    cache_validate_geometry enforces the ONE invariant the retained cache lives or dies by: every
    valid slot owns a DISJOINT reserved quad range.  Slots are packed end-to-end in the shared quad
    arena, so any overlap means a write into one slot (a tessellation, a reuse replay, or a volatile
    patch) silently corrupts another's live geometry -- the exact failure that made the hover
    tooltip flicker/warp when a neighbouring volatile widget patched over it.  These bugs are
    near-invisible after the fact (the command metadata still looks correct; only the record bytes
    are wrong), so the guard runs every frame in debug builds and traps at the source.  Compiled to
    nothing in release (ORB_ASSERT is a no-op there; the whole call is gated too).
==============================================================================================*/

static void
cache_dump_slots( const char* tag )
{
    gui_log( GUI_LOG_INFO, "cached geometry [%s]: %u slots  tess q=%u/%u c=%u/%u",
             tag ? tag : "dump", s_slot_count, s_tess.quad_count, GUI_MAX_QUADS,
             s_tess.cmd_count, GUI_MAX_CMDS );
    for ( u32 i = 0; i < s_slot_count; ++i )
    {
        const win_geo_slot_t* s = &s_slots[ i ];
        gui_log( GUI_LOG_INFO,
                 "  [%2u] win=%-11u z=%-4u vp=%d  quad[%u..%u)/%u  cmd[%u..%u)  gen=%u%s",
                 i, s->win, s->z, s->vp,
                 s->quad_base, s->quad_base + s->quad_count, s->quad_alloc,
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
        if ( sa->quad_count > sa->quad_alloc )
            cache_dump_slots( "OVERFLOW" );
        ORB_ASSERT_MSG( sa->quad_count <= sa->quad_alloc,
                        "gui cache: slot live quad count exceeds its reservation" );

        u32 av0 = sa->quad_base, av1 = sa->quad_base + sa->quad_alloc;

        /* No two slots may share arena space (adjacency at a shared boundary is fine). */
        for ( u32 b = a + 1; b < s_slot_count; ++b )
        {
            const win_geo_slot_t* sb = &s_slots[ b ];
            if ( !sb->valid )
                continue;
            u32  bv0 = sb->quad_base, bv1 = sb->quad_base + sb->quad_alloc;
            bool vhit = ( av0 < bv1 && bv0 < av1 );
            if ( vhit )
                cache_dump_slots( "OVERLAP" );
            ORB_ASSERT_MSG( !vhit, "gui cache: two window slots share quad-arena space" );
        }
    }
}
#endif

/* Host entry point (gui()->debug_dump_geometry): print the current slot table on demand. */
void
build_dump_geometry( void )
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

/* Reuse a window's geometry in place: it sits at prev->quad_base unchanged, so copy the slot
   fields, fold its reservation into the arena tail (max, not assignment -- slots are id-keyed
   and may sit anywhere below the tail), and replay its cached GPU commands at this frame's
   offsets.  `cache_idx` is the window's entry in the id-keyed stable command cache. */
static void
cache_slot_reuse( win_geo_slot_t* slot, const win_geo_slot_t* prev, u32 cache_idx )
{
    slot->quad_base  = prev->quad_base;
    slot->quad_count = prev->quad_count;
    slot->quad_alloc = prev->quad_alloc;
    slot->prim_base  = prev->prim_base;
    slot->prim_count = prev->prim_count;
    slot->prim_alloc = prev->prim_alloc;
    slot->tess_gen   = prev->tess_gen;   /* geometry unchanged: same tessellation pass */
    slot->cache_idx  = cache_idx;        /* id-keyed, so it matches what the quads baked */
    slot->text_quads = prev->text_quads; /* same quads, so the same glyph share */
    slot->text_runs  = prev->text_runs;

    /* The local clip table travels with the geometry it indexes: the cached quads bake
       slot-local clip entries, and these are the rects those entries mean. */
    slot->clip_count = prev->clip_count;
    memcpy( slot->clips, prev->clips, prev->clip_count * sizeof( gui_clip_entry_t ) );

    /* Keep the write head (== the arena tail every fresh tessellation targets) past this slot. */
    if ( prev->quad_base + prev->quad_alloc > s_tess.quad_count )
        s_tess.quad_count = prev->quad_base + prev->quad_alloc;
    if ( prev->prim_base + prev->prim_alloc > s_tess.prim_count )
        s_tess.prim_count = prev->prim_base + prev->prim_alloc;

    /* Replay GPU commands from the stable cache.  lqbase is slot-local, so adding
       slot->quad_base gives the absolute offset for this frame. */
    slot->cmd_base = s_tess.cmd_count;
    u32  nc        = s_win_cached_count[ cache_idx ];
    bool truncated = slot->cmd_base + nc > GUI_MAX_CMDS;

    /* Budget-check the replay like every other gpu_cmds writer (tess_ensure_gpu_cmd bounds the
       tessellation path, volatile_range_close its dormant pads): at cmd-cap saturation an
       unchecked copy would run past the array into the arena counters.  Truncation strands any
       volatile row whose local_cmd_base lies past the cut (the neighbour-rewrite hazard the
       stable-cache write below documents), so the slot also takes a FRESH generation: no capture
       exists for it, every patch generation-check fails safely, and the retire path invalidates
       the window so the next frame re-tessellates through the loudly-warning overflow report. */
    if ( truncated )
    {
        nc               = GUI_MAX_CMDS - slot->cmd_base;
        slot->tess_gen   = ++s_tess_gen_next;
        s_tess.overflow |= TESS_OVF_CMDS;
    }
    slot->cmd_count = nc;
    for ( u32 k = 0; k < nc; ++k )
    {
        u32 ci = slot->cmd_base + k;
        s_tess.gpu_cmds[ ci ].elem_count = s_win_cached[ cache_idx ][ k ].elem_count;
        s_tess.gpu_cmds[ ci ].tex_idx    = s_win_cached[ cache_idx ][ k ].tex_idx;
        s_tess.gpu_cmds[ ci ].vp         = s_win_cached[ cache_idx ][ k ].vp;
        s_tess.gpu_cmds[ ci ].qbase      = (u16)( slot->quad_base + s_win_cached[ cache_idx ][ k ].lqbase );
    }
    s_tess.cmd_count += nc;

    /* This window's own content matched, but that comparison excludes volatile commands entirely
       (cache_diff_windows).  Its volatile rows are patched from this frame's live emit AFTER the
       loop (see reused_volatile_wins) -- once every slot is placed and s_tess.quad_count is the true
       tail, so the patch's scratch tessellation cannot land on a later slot's live geometry.  The
       slot fields (incl. tess_gen) set here are what volatile_patch resolves and generation-checks
       against then. */
    slot->cmd_cached = !truncated;   /* reuse only runs when prev cached its full command run */
    slot->valid      = true;
}

/* Tessellate a changed / new window.  The output always lands at the arena TAIL -- the write head
   past every live reservation (prev and this frame's alike) -- so a tessellation can never write
   over another window's live geometry, whatever it turns out to measure.  Then:

     - fits its own previous reservation -> the geometry is memcpy'd back into that hole (indices
       are slot-relative, so only the slot bases and the absolute qbase shift) and
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
    u32 tail_v = s_tess.quad_count;   /* invariant: the write head is past every live reservation */
    u32 tail_p = s_tess.prim_count;

    slot->quad_base       = tail_v;
    slot->prim_base       = tail_p;
    slot->cmd_base        = s_tess.cmd_count;
    slot->tess_gen        = ++s_tess_gen_next;   /* fresh pass: volatile captures bind to it */
    s_tess.slot_quad_base = tail_v;
    s_tess.slot_prim_base = tail_p;
    s_tess.slot_cmd_base  = s_tess.cmd_count;
    s_tess.slot_tess_gen  = slot->tess_gen;
    s_tess.force_new_cmd  = true;

    /* The dedup floor must not reach back into the PREVIOUS slot's records: their indices are
       relative to that slot's base, so reusing one here would name a record this slot does not
       own.  (This also resets a floor a previous slot's volatile boundary left high.) */
    s_tess.prim_dedup_floor = s_tess.prim_count;
    tess_fx_page_reset();

    /* Fresh tessellation rebuilds the slot's local clip table from scratch (tess_clip_local).
       cache_idx keys the window's fixed clip slab; a window past the cache cap (~0u, dropped
       from caching anyway) borrows slab 0 rather than indexing out of the region. */
    slot->cache_idx          = ( cache_idx == ~0u ) ? 0 : cache_idx;
    slot->clip_count         = 0;
    s_tess.slot_clips        = slot->clips;
    s_tess.slot_clip_count   = &slot->clip_count;
    s_tess.slot_clip_pending = &s_clip_slab_pending[ slot->cache_idx ];
    s_tess.clip_memo_ci      = 0xFF;

    /* Glyph attribution accumulates over exactly this window's pass, then rides the slot. */
    s_tess.slot_text_quads = 0;
    s_tess.slot_text_runs  = 0;

    cache_tess_window( wh );

    slot->text_quads = s_tess.slot_text_quads;
    slot->text_runs  = s_tess.slot_text_runs;

    s_tess.slot_clips        = NULL;
    s_tess.slot_clip_count   = NULL;
    s_tess.slot_clip_pending = NULL;

    slot->quad_count = s_tess.quad_count - slot->quad_base;
    slot->prim_count = s_tess.prim_count - slot->prim_base;
    slot->cmd_count  = s_tess.cmd_count  - slot->cmd_base;

    /* Fresh geometry means fresh records, and (unless it lands back in its own hole below) a
       moved base as well -- either way every region's GPU copy is stale. */
    s_prim_range_pending[ slot->cache_idx ] = 0xFF;

    bool fits = ( prev && prev->valid
                  && slot->quad_count <= prev->quad_alloc
                  && slot->prim_count <= prev->prim_alloc );
    if ( fits )
    {
        /* Relocate from the tail scratch back into the window's own reservation.  The spans are
           disjoint by the tail invariant (tail >= prev base + alloc), so memcpy is safe; only
           the absolute per-command bases shift. */
        u32 dv = tail_v - prev->quad_base;
        u32 dp = tail_p - prev->prim_base;
        if ( dv )
        {
            tess_geo_copy( prev->quad_base, tail_v, slot->quad_count );
            for ( u32 k = 0; k < slot->cmd_count; ++k )
                s_tess.gpu_cmds[ slot->cmd_base + k ].qbase = (u16)( s_tess.gpu_cmds[ slot->cmd_base + k ].qbase - dv );
        }
        /* Records move on their own axis: the record arena is packed independently of the quad
           one, so a slot can land back in its quad hole while its records still shift.  Their
           baked indices are slot-local, so they travel verbatim. */
        if ( dp )
            memcpy( &s_tess.prims[ prev->prim_base ], &s_tess.prims[ tail_p ],
                    slot->prim_count * sizeof( gui_prim_t ) );

        slot->quad_base  = prev->quad_base;
        slot->quad_alloc = prev->quad_alloc;
        slot->prim_base  = prev->prim_base;
        slot->prim_alloc = prev->prim_alloc;

        /* Rewind the tail: the scratch bytes are abandoned, nothing references them. */
        s_tess.quad_count = tail_v;
        s_tess.prim_count = tail_p;
    }
    else
    {
        /* Stays at the tail.  Geometric headroom: a quarter of the live size, floored at the
           fixed pads, so growth relocates logarithmically instead of once per grown frame. */
        u32 pad_v = slot->quad_count / 4u;  if ( pad_v < SLOT_QUAD_PAD ) pad_v = SLOT_QUAD_PAD;
        u32 pad_p = slot->prim_count / 4u;  if ( pad_p < SLOT_PRIM_PAD ) pad_p = SLOT_PRIM_PAD;
        slot->quad_alloc = slot->quad_count + pad_v;
        slot->prim_alloc = slot->prim_count + pad_p;

        /* Clamp to the arena like volatile_range_close: headroom shrinks before the write head
           can pass the cap, so fill ratios and the overflow report never read past the pools. */
        if ( slot->quad_base + slot->quad_alloc > GUI_MAX_QUADS )
            slot->quad_alloc = GUI_MAX_QUADS - slot->quad_base;
        if ( slot->prim_base + slot->prim_alloc > GUI_MAX_PRIMS )
            slot->prim_alloc = GUI_MAX_PRIMS - slot->prim_base;

        s_tess.quad_count = slot->quad_base + slot->quad_alloc;
        s_tess.prim_count = slot->prim_base + slot->prim_alloc;
    }

    /* Only THIS slot's bytes changed (both the in-place and the fresh-tail placement leave every
       other slot untouched), so union its final span into the upload regions' dirty spans rather
       than bumping s_geo_gen -- a steadily-changing window (the focused one, a stats overlay)
       costs its own span per present, not the whole arena.  The repack retry bumps the
       generation instead (cache_build_frame): there every slot moves. */
    patch_span_union( slot->vp, slot->band, slot->quad_base, slot->quad_base + slot->quad_count );

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
        if ( nc > WIN_SLOT_CMD_MAX )
        {
            ++s_tess_stats.uncacheable_wins;

            /* Non-fatal and self-latching, same as the pool-overflow trap below: this window still
               draws correctly (just uncacheable), so the assert exists to flag the cap during dev
               testing, not to treat a normal degrade path as fatal. */
            ORB_ASSERT_MSG_ONCE( false, "gui window exceeded WIN_SLOT_CMD_MAX -- window goes "
                                 "uncacheable and re-tessellates every frame; see the shutdown log "
                                 "for the running count" );
        }
        slot->cmd_cached = false;
        slot->valid      = true;
        return;
    }
    s_win_cached_count[ cache_idx ] = nc;
    for ( u32 k = 0; k < nc; ++k )
    {
        u32 ci = slot->cmd_base + k;
        s_win_cached[ cache_idx ][ k ].elem_count = s_tess.gpu_cmds[ ci ].elem_count;
        s_win_cached[ cache_idx ][ k ].tex_idx    = s_tess.gpu_cmds[ ci ].tex_idx;
        s_win_cached[ cache_idx ][ k ].vp         = s_tess.gpu_cmds[ ci ].vp;
        s_win_cached[ cache_idx ][ k ].lqbase     = (u16)( s_tess.gpu_cmds[ ci ].qbase - slot->quad_base );
    }
    slot->cmd_cached = true;
    slot->valid      = true;
}

/*==============================================================================================
    cache_place_slots (BUILD step 2 driver) -- one placement pass over every window this frame.

    Reuses or tessellates each window (via the two helpers above) and registers it in dispatch.
    allow_reuse false is the repack mode -- prev slots are ignored entirely and the arena packs
    sequentially from 0.  Resets and refills s_slots / s_dispatch / s_tess, so it is safe to run
    twice in one build (cache_build_frame's repack retry).
==============================================================================================*/

static void
cache_place_slots( bool allow_reuse, cache_place_stats_t* st )
{
    memset( st, 0, sizeof( *st ) );
    st->overflow_win = GUI_ID_NONE;

    s_slot_count     = 0;
    s_dispatch_count = 0;

    tess_reset();

    /* The style palette's epoch check, self-gating: a frame whose theme, DPI and atlas layout have
       not moved costs one fold and does nothing.

       An epoch that DROPPED the table forces a full re-place.  A cached slot's quads carry palette
       indices learned against the old table, and the table is emptied rather than patched: entry 12
       under one theme is a different record under the next, so a reused slot would draw the wrong
       shape.  Costs one heavy frame on a theme or DPI change, which already re-tessellates
       everything for its own reasons -- the geometry generation bumps with it so every in-flight
       upload region goes stale. */

    if ( pal_epoch() )
    {
        ++s_geo_gen;
        allow_reuse = false;
    }

    s_tess_stats.band0_quad_end = 0;   /* re-derived below as main-band slots place; 0 when none exist */

    /* Re-derived the same way: summed over the slots this pass places, so the repack retry (which
       runs the whole pass a second time) restates them rather than doubling them. */
    s_tess_stats.text_quads       = 0;
    s_tess_stats.text_runs        = 0;
    s_tess_stats.band0_text_quads = 0;
    s_tess_stats.band0_text_runs  = 0;
    s_tess_stats.live_quads       = 0;
    s_tess_stats.band0_live_quads = 0;

    /* Seed the write head at the arena tail: past every live prev reservation, so a fresh
       tessellation can never land inside geometry a later window still wants to reuse.  In
       repack mode nothing is reused, so packing starts at 0. */
    if ( allow_reuse )
    {
        for ( u32 i = 0; i < s_slot_prev_count; ++i )
        {
            if ( !s_slots_prev[ i ].valid ) continue;
            u32 ve = s_slots_prev[ i ].quad_base + s_slots_prev[ i ].quad_alloc;
            u32 pe = s_slots_prev[ i ].prim_base + s_slots_prev[ i ].prim_alloc;
            if ( ve > s_tess.quad_count ) s_tess.quad_count = ve;
            if ( pe > s_tess.prim_count ) s_tess.prim_count = pe;
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
                st->quad_retained += slot->quad_count;
                ++st->win_retained;
            }
        }
        else
        {
            cache_slot_tessellate( slot, wh, prev, ci );
        }

        /* First window to spill a pool: remember it (and the fills it reached) for the report. */
        if ( !ovf_before && s_tess.overflow && st->overflow_win == GUI_ID_NONE )
        {
            st->overflow_win     = wh->win;
            st->overflow_at_quad = s_tess.quad_count;
            st->overflow_at_cmd  = s_tess.cmd_count;
            st->overflow_at_prim = s_tess.prim_count;
        }

        /* Space this slot owns (live count + retained padding).  Summed over every slot, the tail
           minus this is the dead space the proactive-repack check reclaims. */
        st->reserved_quad += slot->quad_alloc;

        /* Accumulate per-slot geometry stats; exclude self-measuring debug-band windows from totals. */
        s_tess_stats.text_quads += slot->text_quads;
        s_tess_stats.text_runs  += slot->text_runs;
        s_tess_stats.live_quads += slot->quad_count;

        if ( wh->band != 0 )
            ++st->overlay_win;
        else
        {
            s_tess_stats.band0_text_quads += slot->text_quads;
            s_tess_stats.band0_text_runs  += slot->text_runs;
            s_tess_stats.band0_live_quads += slot->quad_count;

            st->total_quad += slot->quad_count;
            st->total_prim += slot->prim_count;

            /* Band boundary: the far edge of the main band's reservations (band-major sort placed
               them first, but id-keyed slots keep historical positions, so track the max extent
               rather than the loop's write head).  The dashboard's memory map draws "main arena
               ends here" at this mark; everything past it is the debug band's own footprint. */
            u32 ve = slot->quad_base + slot->quad_alloc;
            if ( ve > s_tess_stats.band0_quad_end ) s_tess_stats.band0_quad_end = ve;
        }

        s_dispatch[ s_dispatch_count++ ] = slot;
    }
}

// clang-format on
/*============================================================================================*/
