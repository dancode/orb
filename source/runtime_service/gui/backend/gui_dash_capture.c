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

    sn->tess_verts     = s_tess.vert_count;   sn->tess_idx = s_tess.idx_count;   /* tess_cmds below */
    sn->vert_hwm       = s_tess_stats.vert_hwm;     sn->idx_hwm  = s_tess_stats.idx_hwm;
    sn->overflow_ever  = s_tess_stats.overflow_ever;
    sn->band0_vert_end = s_tess_stats.band0_vert_end;
    sn->band0_idx_end  = s_tess_stats.band0_idx_end;
    sn->band0_vert_hwm = s_tess_stats.band0_vert_hwm;
    sn->band0_idx_hwm  = s_tess_stats.band0_idx_hwm;

    /* GPU DRAW commands per band -- what actually dispatches, matching the renderer's draw-call
       count and the perf tracker.  A slot's cmd_count includes dormant volatile-pad commands
       (vp == GUI_VP_INVALID) and can hold empty commands; both occupy pool slots but never draw,
       so they are excluded here (as the batch inspector already does).  tess_cmds is the live total
       across both bands; tess_cmds_dbg the debug band's share.  band-0 draws = total - debug. */
    sn->tess_cmds = sn->tess_cmds_dbg = 0;
    for ( u32 i = 0; i < sn->slot_count; ++i )
    {
        const win_geo_slot_t* sl = &s_slots[ i ];
        if ( !sl->valid ) continue;
        u32 live = 0;
        for ( u32 k = 0; k < sl->cmd_count && sl->cmd_base + k < s_tess.cmd_count; ++k )
        {
            u32 ci = sl->cmd_base + k;
            if ( s_tess.cmd_vp[ ci ] == GUI_VP_INVALID ) continue;   /* dormant volatile pad     */
            if ( s_tess.cmds[ ci ].elem_count == 0 )    continue;    /* empty -- never dispatched */
            ++live;
        }
        sn->tess_cmds += live;
        if ( sl->band != 0 )
            sn->tess_cmds_dbg += live;
    }

    sn->emit_cmds     = s_draw.cmd_count;    sn->emit_segs  = s_draw.seg_count;
    sn->emit_pts      = s_draw.pt_count;     sn->emit_text  = s_draw.text_pool_used;
    sn->emit_clips    = s_draw.clip_table_n; sn->emit_rects = s_draw.rect_count;
    if ( sn->emit_cmds > sn->emit_cmds_hwm )   /* emit cmd pool has no backend hwm -- track it here */
        sn->emit_cmds_hwm = sn->emit_cmds;

    /* Debug-band attribution of every shared emit pool: what the diagnostic UI itself consumed this
       frame, so the shell can subtract it and show a real application's use limits.  Derived here
       from the segment table (which carries the band) plus each command's own payload -- the emit
       hot paths carry no per-band counters.  A clip rect is charged to the debug band when ANY
       debug-band command references it: the clip table is shared + de-duplicated, so a rect a
       diagnostic window shares with a real window (e.g. a viewport window's clip that the torn-off
       dashboard reuses) only exists in this frame's picture because the observer is present -- it is
       not a cost the application would carry on its own. */
    sn->emit_cmds_dbg = sn->emit_segs_dbg = 0;
    sn->emit_pts_dbg  = sn->emit_text_dbg = 0;
    sn->emit_rects_dbg = 0;

    u8 clip_band[ GUI_MAX_CLIP_RECTS ] = { 0 };   /* bit0 = used by band 0, bit1 = used by debug band */
    for ( u32 si = 0; si < s_draw.seg_count; ++si )
    {
        const gui_cmd_seg_t* sg  = &s_draw.segs[ si ];
        bool                 dbg = ( sg->band != 0 );
        if ( dbg && sg->hi > sg->lo )
        {
            ++sn->emit_segs_dbg;
            sn->emit_cmds_dbg += sg->hi - sg->lo;
        }
        for ( u32 ci = sg->lo; ci < sg->hi && ci < GUI_MAX_CMDS; ++ci )
        {
            const gui_cmd_t* c = &s_draw.cmds[ ci ];
            if ( dbg )
            {
                if ( c->type == GUI_CMD_TEXT )      sn->emit_text_dbg  += c->text.len;
                if ( c->type == GUI_CMD_POLYLINE )  sn->emit_pts_dbg   += c->polyline.pt_count;
                if ( c->type == GUI_CMD_RECT_LIST ) sn->emit_rects_dbg += c->rect_list.count;
            }
            clip_band[ c->clip_idx ] |= dbg ? 0x2u : 0x1u;
        }
    }
    sn->emit_clips_dbg = 0;
    for ( u32 i = 0; i < s_draw.clip_table_n && i < GUI_MAX_CLIP_RECTS; ++i )
        if ( clip_band[ i ] & 0x2u )               /* touched by the debug band at all */
            ++sn->emit_clips_dbg;

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
