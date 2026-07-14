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
    Inspector read seam -- resolve one frozen command / segment for the shell (gui_step_window.c).
    Lives backend-side because the frozen pools do: polyline points, rect-list entries and text
    bytes are only reachable here.
----------------------------------------------------------------------------------------------*/

/* Pixel bbox of one frozen command -- the highlight box the shell outlines.  Fills/outlines are
   exact; strokes pad by half thickness; TEXT walks the ACTIVE font's advances (a run frozen in
   another font measures approximately -- acceptable for a highlight aid). */
static gui_rect_t
step_cmd_bounds( const gui_cmd_t* c )
{
    switch ( (gui_cmd_type_t)c->type )
    {
        case GUI_CMD_RECT_FILLED:
            return ( gui_rect_t ){ c->rect.x, c->rect.y, c->rect.w, c->rect.h };
        case GUI_CMD_RECT_OUTLINE:
            return ( gui_rect_t ){ c->rect_outline.x, c->rect_outline.y,
                                   c->rect_outline.w, c->rect_outline.h };
        case GUI_CMD_RECT_GRADIENT:
            return ( gui_rect_t ){ c->gradient.x, c->gradient.y, c->gradient.w, c->gradient.h };
        case GUI_CMD_TRIANGLE:
        {
            f32 x0 = c->tri.ax, x1 = c->tri.ax, y0 = c->tri.ay, y1 = c->tri.ay;
            if ( c->tri.bx < x0 ) x0 = c->tri.bx;   if ( c->tri.bx > x1 ) x1 = c->tri.bx;
            if ( c->tri.cx < x0 ) x0 = c->tri.cx;   if ( c->tri.cx > x1 ) x1 = c->tri.cx;
            if ( c->tri.by < y0 ) y0 = c->tri.by;   if ( c->tri.by > y1 ) y1 = c->tri.by;
            if ( c->tri.cy < y0 ) y0 = c->tri.cy;   if ( c->tri.cy > y1 ) y1 = c->tri.cy;
            return ( gui_rect_t ){ x0, y0, x1 - x0, y1 - y0 };
        }
        case GUI_CMD_TEXT:
        {
            const char* s = s_step.text_pool + c->text.off;
            f32         w = 0.0f;
            for ( u32 i = 0; i < c->text.len && s[ i ]; ++i )
            {
                f32 u0, v0, u1, v1, ox, oy, gw, gh, adv;
                font_glyph( (u8)s[ i ], &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &adv );
                w += adv;
            }
            /* Fold the glyph-level hard-clip window in, when one was baked. */
            f32 x0 = c->text.x, x1 = c->text.x + w;
            if ( c->text.clip_x0 > x0 ) x0 = c->text.clip_x0;
            if ( c->text.clip_x1 < x1 ) x1 = c->text.clip_x1;
            return ( gui_rect_t ){ x0, c->text.y, x1 > x0 ? x1 - x0 : 0.0f, font_line_h() };
        }
        case GUI_CMD_CIRCLE_FILLED:
            return ( gui_rect_t ){ c->circle.cx - c->circle.r, c->circle.cy - c->circle.r,
                                   2.0f * c->circle.r, 2.0f * c->circle.r };
        case GUI_CMD_LINE:
        case GUI_CMD_DASHED_LINE:
        {
            /* line and dash share the same leading x0..thickness layout. */
            f32 t  = c->line.thickness * 0.5f + 1.0f;
            f32 x0 = c->line.x0 < c->line.x1 ? c->line.x0 : c->line.x1;
            f32 x1 = c->line.x0 < c->line.x1 ? c->line.x1 : c->line.x0;
            f32 y0 = c->line.y0 < c->line.y1 ? c->line.y0 : c->line.y1;
            f32 y1 = c->line.y0 < c->line.y1 ? c->line.y1 : c->line.y0;
            return ( gui_rect_t ){ x0 - t, y0 - t, ( x1 - x0 ) + 2.0f * t, ( y1 - y0 ) + 2.0f * t };
        }
        case GUI_CMD_POLYLINE:
        {
            const gui_vec2_t* p = s_step.points + c->polyline.pt_offset;
            f32 x0 = p[ 0 ].x, x1 = p[ 0 ].x, y0 = p[ 0 ].y, y1 = p[ 0 ].y;
            for ( u32 i = 1; i < c->polyline.pt_count; ++i )
            {
                if ( p[ i ].x < x0 ) x0 = p[ i ].x;   if ( p[ i ].x > x1 ) x1 = p[ i ].x;
                if ( p[ i ].y < y0 ) y0 = p[ i ].y;   if ( p[ i ].y > y1 ) y1 = p[ i ].y;
            }
            f32 t = c->polyline.thickness * 0.5f + 1.0f;
            return ( gui_rect_t ){ x0 - t, y0 - t, ( x1 - x0 ) + 2.0f * t, ( y1 - y0 ) + 2.0f * t };
        }
        case GUI_CMD_RECT_LIST:
        {
            const gui_rect_col_t* r = s_step.rect_pool + c->rect_list.offset;
            f32 x0 = r[ 0 ].x, y0 = r[ 0 ].y, x1 = r[ 0 ].x + r[ 0 ].w, y1 = r[ 0 ].y + r[ 0 ].h;
            for ( u32 i = 1; i < c->rect_list.count; ++i )
            {
                if ( r[ i ].x < x0 )            x0 = r[ i ].x;
                if ( r[ i ].y < y0 )            y0 = r[ i ].y;
                if ( r[ i ].x + r[ i ].w > x1 ) x1 = r[ i ].x + r[ i ].w;
                if ( r[ i ].y + r[ i ].h > y1 ) y1 = r[ i ].y + r[ i ].h;
            }
            return ( gui_rect_t ){ x0, y0, x1 - x0, y1 - y0 };
        }
    }
    return ( gui_rect_t ){ 0.0f, 0.0f, 0.0f, 0.0f };
}

bool
gui_step_cmd_info( u32 index, step_cmd_info_t* out )
{
    if ( !g_gui_step_frozen || index >= s_step.cmd_count )
        return false;

    const gui_cmd_t* c = &s_step.cmds[ index ];
    out->cmd    = *c;
    out->bounds = step_cmd_bounds( c );
    out->clip   = s_step.clip_table[ c->clip_idx ];
    out->text   = ( c->type == GUI_CMD_TEXT ) ? s_step.text_pool + c->text.off : NULL;

    /* Owning segment tag (linear scan: segments are few and this runs per shell query only). */
    out->win = 0;  out->z = 0;  out->vp = 0;  out->font = 0;
    for ( u32 si = 0; si < s_step.seg_count; ++si )
        if ( index >= s_step.segs[ si ].lo && index < s_step.segs[ si ].hi )
        {
            out->win  = s_step.segs[ si ].win;
            out->z    = s_step.segs[ si ].z;
            out->vp   = s_step.segs[ si ].vp;
            out->font = s_step.segs[ si ].font;
            break;
        }
    return true;
}

u32
gui_step_seg_count( void )
{
    return g_gui_step_frozen ? s_step.seg_count : 0;
}

bool
gui_step_seg_info( u32 index, step_seg_info_t* out )
{
    if ( !g_gui_step_frozen || index >= s_step.seg_count )
        return false;

    const gui_cmd_seg_t* sg = &s_step.segs[ index ];
    out->win  = sg->win;
    out->z    = sg->z;
    out->vp   = sg->vp;
    out->font = sg->font;
    out->lo   = sg->lo;
    out->hi   = sg->hi;

    /* Bounds: union of the member commands' bboxes. */
    gui_rect_t u = ( gui_rect_t ){ 0.0f, 0.0f, 0.0f, 0.0f };
    for ( u32 i = sg->lo; i < sg->hi; ++i )
    {
        gui_rect_t b = step_cmd_bounds( &s_step.cmds[ i ] );
        if ( b.w <= 0.0f || b.h <= 0.0f )
            continue;
        if ( u.w <= 0.0f )
            u = b;
        else
        {
            f32 x1 = u.x + u.w > b.x + b.w ? u.x + u.w : b.x + b.w;
            f32 y1 = u.y + u.h > b.y + b.h ? u.y + u.h : b.y + b.h;
            if ( b.x < u.x ) u.x = b.x;
            if ( b.y < u.y ) u.y = b.y;
            u.w = x1 - u.x;
            u.h = y1 - u.y;
        }
    }
    out->bounds = u;
    return true;
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
