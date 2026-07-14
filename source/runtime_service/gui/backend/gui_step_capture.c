/*==============================================================================================

    runtime_service/gui/backend/gui_step_capture.c -- Command stepper: frozen-frame capture + replay.

    Freeze one frame's semantic command list and replay a PREFIX of it, so the UI's generation
    can be stepped command by command.  Two halves, both here:

        step_capture_build -- start of cache_build_frame (segments closed, pools complete):
                              copies the frame's band-0 segments -- commands and hashes compacted
                              contiguously, segment ranges rebased -- plus every side pool WHOLE
                              (clip table, text pool, point pool, rect pool), so each pool offset
                              baked into a command stays valid verbatim.  Sets g_gui_step_frozen.
        step_restore_emit  -- end of draw_reset, while frozen: pre-loads s_draw with the frozen
                              commands [0, cursor), the frozen pools, and a clamped copy of the
                              frozen segment table, then opens a fresh live tail span.  Live
                              emission continues after the frozen content -- but every band-0
                              push is suppressed at the source (STEP_EMIT_SUPPRESSED in
                              gui_emit_draw.c), so only the debug band (the stepper's own
                              controls, the dashboard, the overlays) lands on top.

    No replay-specific tessellation exists: the restored s_draw IS the frame, so the ordinary
    pipeline -- hash diff, retained cache, per-window slots, z-sorted dispatch -- builds and
    renders it unchanged.  A cursor change alters the restored commands, the diff flags exactly
    the affected windows, and only those re-tessellate; an untouched frozen frame hash-matches
    and idle-skips like any clean frame.

    Volatile widgets: cmd_volatile_id is NOT captured -- draw_reset zeroes it, so frozen volatile
    commands replay as plain static content and (unlike live frames) fold into their window's
    hash.  That first-freeze hash shift retessellates volatile-carrying windows once; from then
    on the frozen frame is static.  The live registry's rows go stale meanwhile (their tess_gen
    no longer matches any slot) so every patch attempt refuses by the generation check; on
    release the live emit re-tags its commands and the rows force a recapture, self-healing.

    State transitions latch and apply only at frame seams -- capture at the build, release at the
    next draw_reset -- so a frame is never half live, half frozen.  Each mutating call site is
    responsible for wants_redraw (debug_hotkeys sets it), keeping the seek -> restore handoff
    ahead of the clean-frame emit skip.

    Included by gui_backend.c LAST (after gui_dash_capture.c) so every emit static it copies
    (s_draw and its pools) is in scope.  Compiled out unless GUI_CMD_STEPPER (gui_backend.h);
    the pipeline hooks compile to (void)0 then.

==============================================================================================*/
// clang-format off

#ifdef GUI_CMD_STEPPER

/* Read by the emit hot path (STEP_EMIT_SUPPRESSED) to suppress live band-0 pushes per frame. */
bool g_gui_step_frozen;

static struct
{
    bool want_capture;   /* latched by gui_step_capture(); taken at the next cache_build_frame  */
    bool want_release;   /* latched by gui_step_release(); applied at the next draw_reset       */
    u32  cursor;         /* replay shows frozen commands [0, cursor)                            */

    /* The frozen frame.  Band-0 commands/hashes compacted contiguously with rebased segments;
       side pools copied whole (see the file banner). */
    gui_cmd_t      cmds      [ GUI_MAX_CMDS ];         u32 cmd_count;
    u32            cmd_hashes[ GUI_MAX_CMDS ];
    gui_cmd_seg_t  segs      [ GUI_MAX_SEGS ];         u32 seg_count;
    gui_rect_t     clip_table[ GUI_MAX_CLIP_RECTS ];
    u32            clip_hash [ GUI_MAX_CLIP_RECTS ];   u32 clip_n;
    gui_vec2_t     points    [ GUI_MAX_PATH_PTS ];     u32 pt_count;
    gui_rect_col_t rect_pool [ GUI_MAX_RECT_ENTRIES ]; u32 rect_count;
    char           text_pool [ GUI_MAX_TEXT_POOL ];    u32 text_used;

} s_step;

/*----------------------------------------------------------------------------------------------
    Shell seam (gui_backend.h) -- state requests latch here; the pipeline hooks below apply them
    at the frame seams.
----------------------------------------------------------------------------------------------*/

void gui_step_capture( void )  { s_step.want_capture = true; }
void gui_step_release( void )  { s_step.want_release = true; }
bool gui_step_frozen ( void )  { return g_gui_step_frozen; }
u32  gui_step_count  ( void )  { return g_gui_step_frozen ? s_step.cmd_count : 0; }
u32  gui_step_cursor ( void )  { return s_step.cursor; }

void
gui_step_seek( u32 cursor )
{
    s_step.cursor = cursor < s_step.cmd_count ? cursor : s_step.cmd_count;
}

/*----------------------------------------------------------------------------------------------
    step_capture_build -- start of cache_build_frame: the final segment is closed, every pool is
    complete, nothing has been diffed or tessellated yet.  Copies the live frame and freezes.
----------------------------------------------------------------------------------------------*/

void
step_capture_build( void )
{
    if ( !s_step.want_capture )
        return;
    s_step.want_capture = false;
    if ( g_gui_step_frozen )
        return;   /* already frozen -- a second capture would snapshot the replay itself */

    /* Band-0 segments only: the debug band is the stepper's own machinery (plus the dashboard /
       overlays) and must never appear in its own replay.  Commands and their emit-time hashes
       compact to the front; each segment's [lo, hi) rebases to the compacted positions. */
    u32 w = 0, ns = 0;
    for ( u32 si = 0; si < s_draw.seg_count; ++si )
    {
        const gui_cmd_seg_t* sg = &s_draw.segs[ si ];
        if ( sg->band != 0 || sg->lo == sg->hi )
            continue;
        if ( ns >= GUI_MAX_SEGS - 1u )
            break;   /* keep one slot free for the restore's live tail span */
        u32 n = sg->hi - sg->lo;
        memcpy( s_step.cmds       + w, s_draw.cmds       + sg->lo, n * sizeof( gui_cmd_t ) );
        memcpy( s_step.cmd_hashes + w, s_draw.cmd_hashes + sg->lo, n * sizeof( u32 ) );
        s_step.segs[ ns ]    = *sg;
        s_step.segs[ ns ].lo = w;
        s_step.segs[ ns ].hi = w + n;
        ++ns;
        w += n;
    }
    s_step.cmd_count = w;
    s_step.seg_count = ns;

    /* Side pools copied whole: pool offsets baked into the commands stay valid verbatim, and
       the pools may hold debug-band content interleaved -- unreferenced entries are inert. */
    memcpy( s_step.clip_table, s_draw.clip_table,      s_draw.clip_table_n * sizeof( gui_rect_t ) );
    memcpy( s_step.clip_hash,  s_draw.clip_hash_cache, s_draw.clip_table_n * sizeof( u32 ) );
    s_step.clip_n = s_draw.clip_table_n;
    memcpy( s_step.points,     s_draw.points,          s_draw.pt_count * sizeof( gui_vec2_t ) );
    s_step.pt_count = s_draw.pt_count;
    memcpy( s_step.rect_pool,  s_draw.rect_pool,       s_draw.rect_count * sizeof( gui_rect_col_t ) );
    s_step.rect_count = s_draw.rect_count;
    memcpy( s_step.text_pool,  s_draw.text_pool,       s_draw.text_pool_used );
    s_step.text_used = s_draw.text_pool_used;

    /* Freeze showing the whole frame: the first frozen build is visually identical to the live
       frame it replaces (only volatile-carrying windows retess once -- see the file banner). */
    s_step.cursor     = w;
    g_gui_step_frozen = true;
}

/*----------------------------------------------------------------------------------------------
    step_restore_emit -- end of draw_reset: the frame state is freshly seeded (background segment
    open, root clip at slot 0, volatile tags zeroed).  Applies a pending release, then while
    frozen replaces the empty frame with the frozen prefix.
----------------------------------------------------------------------------------------------*/

void
step_restore_emit( void )
{
    if ( s_step.want_release )
    {
        s_step.want_release = false;
        g_gui_step_frozen   = false;   /* this frame emits live again; the hash diff retesses any
                                          window whose live content differs from the frozen prefix */
    }
    if ( !g_gui_step_frozen )
        return;

    u32 cur = s_step.cursor;   /* clamped at seek time */

    /* The visible command prefix, with its emit-time hashes.  cmd_volatile_id stays zeroed
       (draw_reset's memset) -- frozen volatile content replays as plain static commands. */
    memcpy( s_draw.cmds,       s_step.cmds,       cur * sizeof( gui_cmd_t ) );
    memcpy( s_draw.cmd_hashes, s_step.cmd_hashes, cur * sizeof( u32 ) );
    s_draw.cmd_count = cur;

    /* Side pools whole, so every pool offset in the prefix resolves; live debug-band emission
       appends past the frozen counts and the two never collide. */
    memcpy( s_draw.points,    s_step.points,    s_step.pt_count * sizeof( gui_vec2_t ) );
    memcpy( s_draw.rect_pool, s_step.rect_pool, s_step.rect_count * sizeof( gui_rect_col_t ) );
    memcpy( s_draw.text_pool, s_step.text_pool, s_step.text_used );
    s_draw.pt_count       = s_step.pt_count;
    s_draw.rect_count     = s_step.rect_count;
    s_draw.text_pool_used = s_step.text_used;

    /* Frozen clip table at its original indices, then ONE fresh live root appended after it:
       the display rect draw_reset just seeded, so the debug band scissors against the CURRENT
       surface size even if the window was resized while frozen. */
    u32 n = s_step.clip_n;
    memcpy( s_draw.clip_table,      s_step.clip_table, n * sizeof( gui_rect_t ) );
    memcpy( s_draw.clip_hash_cache, s_step.clip_hash,  n * sizeof( u32 ) );
    if ( n < GUI_MAX_CLIP_RECTS - 1u )
    {
        gui_rect_t root             = s_draw.clip_stack[ 0 ];
        s_draw.clip_table[ n ]      = root;
        s_draw.clip_hash_cache[ n ] = fnv1a( 2166136261u, &root, sizeof( gui_rect_t ) );
        s_draw.clip_idx_stack[ 0 ]  = (u8)n;
        s_draw.cur_clip_idx         = (u8)n;
        s_draw.clip_table_n         = n + 1;
    }
    else
    {
        s_draw.clip_idx_stack[ 0 ] = 0;   /* table full: fall back to the frozen root (slot 0) */
        s_draw.cur_clip_idx        = 0;
        s_draw.clip_table_n        = n;
    }

    /* Frozen segments clamped to the cursor (they are ordered by lo, so the scan stops at the
       first span past it), then the live tail: an open background-tagged span from `cur`,
       matching the ambient state draw_reset seeded -- the debug band re-tags from here. */
    u32 ns = 0;
    for ( u32 si = 0; si < s_step.seg_count; ++si )
    {
        if ( s_step.segs[ si ].lo >= cur )
            break;
        s_draw.segs[ ns ] = s_step.segs[ si ];
        if ( s_draw.segs[ ns ].hi > cur )
            s_draw.segs[ ns ].hi = cur;
        ++ns;
    }
    s_draw.segs[ ns++ ] = ( gui_cmd_seg_t ){ 0, 0, 0, s_draw.cur_font, 0, cur, cur };
    s_draw.seg_count    = ns;
}

#endif /* GUI_CMD_STEPPER */

// clang-format on
/*============================================================================================*/
