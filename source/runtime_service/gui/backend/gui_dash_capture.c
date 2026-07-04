/*==============================================================================================

    runtime_service/gui/backend/gui_dash_capture.c -- Pipeline dashboard snapshot capture.

    The backend half of the pipeline diagnostic dashboard (see gui_dashboard.c for the window
    and every panel painter).  Copies a coherent snapshot of the pipeline at two defined points:

        dash_capture_build -- end of cache_build_frame: slot table (with arena bands), dispatch
                              order, tessellated GPU commands, volatile registry, emit counters,
                              diff verdicts, published stats.
        dash_capture_flush -- end of each surface's gui_render_flush: frame-in-flight index,
                              upload spans, upload bytes/batches, draw calls.

    The shell reads the snapshot through gui_dash_snapshot() (gui_backend.h) and draws the
    panels itself with the standard draw API, as an ordinary GUI_WIN_DEBUG_BAND window: the
    band system packs its geometry after every main-band slot and keeps it out of the stats
    and any_changed signals, so the dashboard never pollutes the arena layout or the metrics
    it displays.  Because the shell emits one frame after a capture, the display lags the
    pipeline by one frame -- the standard self-measurement lag.

    Included by gui_backend.c LAST so every pipeline static it reads (s_draw, s_tess,
    s_slots/s_dispatch/s_cache/s_stats, s_volatile, s_tess_gen_next) is in scope.  Compiled out
    unless GUI_PIPELINE_DASHBOARD (gui_backend.h); the capture hooks compile to (void)0 then.

==============================================================================================*/
// clang-format off

#ifdef GUI_PIPELINE_DASHBOARD

static struct
{
    bool            enabled;   /* shell is open; gates both captures (a closed dash costs a branch) */
    bool            freeze;    /* captures halted; the snapshot holds for inspection */
    dash_snapshot_t snap;

} s_dash;

/* End of cache_build_frame: every slot is placed, dispatch is z-sorted, stats are accumulated. */
void
dash_capture_build( void )
{
    if ( !s_dash.enabled || s_dash.freeze )
        return;

    dash_snapshot_t* sn = &s_dash.snap;
    sn->serial++;

    sn->slot_count = s_slot_count < RENDER_MAX_WIN ? s_slot_count : RENDER_MAX_WIN;
    for ( u32 i = 0; i < sn->slot_count; ++i )
    {
        const win_geo_slot_t* sl = &s_slots[ i ];
        dash_slot_t*          d  = &sn->slots[ i ];
        d->win        = sl->win;
        d->z          = sl->z;          d->vp        = sl->vp;         d->band = sl->band;
        d->vert_base  = sl->vert_base;  d->vert_count = sl->vert_count;  d->vert_alloc = sl->vert_alloc;
        d->idx_base   = sl->idx_base;   d->idx_count  = sl->idx_count;   d->idx_alloc  = sl->idx_alloc;
        d->cmd_base   = sl->cmd_base;   d->cmd_count  = sl->cmd_count;
        d->tess_gen   = sl->tess_gen;   d->valid      = sl->valid;

        d->changed = false;
        for ( u32 c = 0; c < s_cache.cur_n; ++c )
            if ( s_cache.cur[ c ].win == sl->win ) { d->changed = s_cache.cur[ c ].changed; break; }
    }

    sn->dispatch_count = s_dispatch_count < RENDER_MAX_WIN ? s_dispatch_count : RENDER_MAX_WIN;
    for ( u32 d = 0; d < sn->dispatch_count; ++d )
        sn->dispatch[ d ] = (u8)( s_dispatch[ d ] - s_slots );

    sn->cmd_count = s_tess.cmd_count < GUI_MAX_CMDS ? s_tess.cmd_count : GUI_MAX_CMDS;
    for ( u32 c = 0; c < sn->cmd_count; ++c )
    {
        sn->cmds[ c ].elem_count = s_tess.cmds     [ c ].elem_count;
        sn->cmds[ c ].tex_idx    = s_tess.cmds     [ c ].tex_idx;
        sn->cmds[ c ].clip       = s_tess.cmds     [ c ].clip_rect;
        sn->cmds[ c ].vp         = s_tess.cmd_vp   [ c ];
        sn->cmds[ c ].vbase      = s_tess.cmd_vbase[ c ];
        sn->cmds[ c ].ibase      = s_tess.cmd_ibase[ c ];
    }

    sn->vol_count = s_volatile_count < GUI_MAX_VOLATILE ? s_volatile_count : GUI_MAX_VOLATILE;
    for ( u32 v = 0; v < sn->vol_count; ++v )
    {
        const gui_volatile_slot_t* vs = &s_volatile[ v ];
        dash_vol_t*                d  = &sn->vols[ v ];
        d->id         = vs->id;               d->win        = vs->win;
        d->tess_gen   = vs->tess_gen;
        d->lvert_base = vs->local_vert_base;  d->vert_count = vs->vert_count;  d->vert_alloc = vs->vert_alloc;
        d->lidx_base  = vs->local_idx_base;   d->idx_count  = vs->idx_count;   d->idx_alloc  = vs->idx_alloc;
        d->cmd_count  = vs->cmd_count;        d->cmd_alloc  = vs->cmd_alloc;
        d->active     = vs->active;           d->hidden     = vs->hidden;
    }

    sn->tess_verts     = s_tess.vert_count;   sn->tess_idx = s_tess.idx_count;
    sn->tess_cmds      = s_tess.cmd_count;
    sn->vert_hwm       = s_tess.vert_hwm;     sn->idx_hwm  = s_tess.idx_hwm;
    sn->overflow_ever  = s_tess.overflow_ever;
    sn->band0_vert_end = s_tess.band0_vert_end;
    sn->band0_idx_end  = s_tess.band0_idx_end;

    sn->emit_cmds     = s_draw.cmd_count;    sn->emit_segs  = s_draw.seg_count;
    sn->emit_pts      = s_draw.pt_count;     sn->emit_text  = s_draw.text_pool_used;
    sn->emit_clips    = s_draw.clip_table_n;

    /* Debug-band attribution: how much of the shared emit command pool the diagnostic UI itself
       consumed this frame.  Derived from the segment table at capture time -- the emit hot paths
       carry no per-band counters. */
    sn->emit_cmds_dbg = 0;
    for ( u32 si = 0; si < s_draw.seg_count; ++si )
        if ( s_draw.segs[ si ].band != 0 && s_draw.segs[ si ].hi > s_draw.segs[ si ].lo )
            sn->emit_cmds_dbg += s_draw.segs[ si ].hi - s_draw.segs[ si ].lo;

    sn->diff_unchanged = s_cache.unchanged;  sn->any_changed = s_cache.any_changed;
    sn->tess_gen_next  = s_tess_gen_next;
    sn->font_atlas     = font_atlas_idx();

    sn->stats          = s_stats.published;
    sn->draw_call_hwm  = s_stats.draw_call_hwm;
}

/* End of one surface's gui_render_flush: what physically hit the GPU for that surface.  Runs
   every frame (real or idle) since cached geometry is replayed regardless. */
void
dash_capture_flush( u32 vp, u32 frame, u32 vtx_lo, u32 vtx_hi, u32 idx_lo, u32 idx_hi,
                    u32 bytes, u32 batches, u32 draws )
{
    if ( !s_dash.enabled || s_dash.freeze || vp >= GUI_MAX_VIEWPORTS )
        return;
    s_dash.snap.surf[ vp ] = ( dash_surf_t ){
        .live = true, .frame_index = frame,
        .vtx_lo = vtx_lo, .vtx_hi = vtx_hi, .idx_lo = idx_lo, .idx_hi = idx_hi,
        .up_bytes = bytes, .up_batches = batches, .draw_calls = draws,
    };
}

/*==============================================================================================
    Shell seam (called from gui_dashboard.c, UI unit)
==============================================================================================*/

const dash_snapshot_t* gui_dash_snapshot   ( void )    { return &s_dash.snap; }
void                   gui_dash_set_enabled( bool on ) { s_dash.enabled = on; }
void                   gui_dash_set_freeze ( bool on ) { s_dash.freeze = on; }
bool                   gui_dash_frozen     ( void )    { return s_dash.freeze; }

#endif /* GUI_PIPELINE_DASHBOARD */

// clang-format on
/*============================================================================================*/
