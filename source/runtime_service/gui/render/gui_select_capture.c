/*==============================================================================================

    runtime_service/gui/render/gui_select_capture.c -- Text-run capture for text selection.

    The backend half of window text selection (GUI_WIN_TEXT_SELECT); the UI half (the
    selection controller: press/drag protocol, highlight paint, Ctrl+C copy) is
    chrome/window/gui_select.c (the chrome unit).  See the section banner in gui_render.h for the boundary.

    At the build seam -- segments closed, every emit pool complete, nothing tessellated yet
    (the same seam the command stepper freezes at) -- the GUI_CMD_TEXT commands of each
    marked window are copied (position, byte range, segment font/vp, clip) into a run buffer
    that survives the next draw_reset.  The controller works against LAST frame's runs: one
    frame of self-measurement lag, invisible because window content is static while the user
    sweeps a selection.

    ONE shared buffer serves every flagged window (runs tag their owning window id): marks
    are re-registered by every flagged gui_window_end on every emitted frame and consumed
    here, and the buffer is rebuilt whole -- so it always mirrors the last real emit and
    persists verbatim across clean (idle-skipped) frames, exactly like the geometry it
    describes.  A flagged window that stops emitting (closed, collapsed) simply stops
    marking, and its runs drop at the next capture.

    Included LAST in gui_render.c (with the dash / step captures) so s_draw is in scope.

==============================================================================================*/
// clang-format off

#define GUI_SELECT_MARK_MAX 8    /* flagged windows capturable per frame */

static struct
{
    gui_id_t marks[ GUI_SELECT_MARK_MAX ];   /* windows to capture at this frame's build */
    u32      mark_count;

    gui_select_run_t runs[ GUI_SELECT_MAX_RUNS ];
    u32              run_count;
    char             text [ GUI_SELECT_TEXT_POOL ];
    u32              text_used;

    u32 serial;    /* bumped per capture; the controller revalidates its endpoints on change */

} s_select_cap;

/*============================================================================================*/
/* Flag `win` for capture at this frame's build seam.  Called by gui_window_end for every
   expanded GUI_WIN_TEXT_SELECT window, every emitted frame; consumed by select_capture_build. */

void
select_capture_mark( gui_id_t win )
{
    if ( win == 0 )
        return;
    for ( u32 i = 0; i < s_select_cap.mark_count; ++i )
        if ( s_select_cap.marks[ i ] == win )
            return;
    if ( s_select_cap.mark_count < GUI_SELECT_MARK_MAX )
        s_select_cap.marks[ s_select_cap.mark_count++ ] = win;
}

/*============================================================================================*/
/* Rebuild the run buffer from this frame's command list.  Called once per real build from
   cache_build_frame, right after the final segment is closed.  Walks the segment table (not
   the raw command list) so only marked windows' spans are touched; the debug band is skipped
   -- diagnostic text is not selectable.  Buffer overflow (runs or text bytes) drops the
   remainder rather than failing: selection simply does not extend past the cap. */

void
select_capture_build( void )
{
    /* Feature idle: nothing marked and nothing lingering from a previous capture. */
    if ( s_select_cap.mark_count == 0 && s_select_cap.run_count == 0 )
        return;

    s_select_cap.run_count = 0;
    s_select_cap.text_used = 0;
    ++s_select_cap.serial;

    for ( u32 s = 0; s < s_draw.seg_count; ++s )
    {
        const gui_cmd_seg_t* seg = &s_draw.segs[ s ];
        if ( seg->band != 0 || seg->win == 0 )
            continue;

        bool marked = false;
        for ( u32 m = 0; m < s_select_cap.mark_count; ++m )
            if ( s_select_cap.marks[ m ] == seg->win ) { marked = true; break; }
        if ( !marked )
            continue;

        for ( u32 c = seg->lo; c < seg->hi; ++c )
        {
            const gui_cmd_t* cmd = &s_draw.cmds[ c ];
            if ( cmd->type != GUI_CMD_TEXT || cmd->text.len == 0 )
                continue;
            if ( s_select_cap.run_count >= GUI_SELECT_MAX_RUNS
              || s_select_cap.text_used + cmd->text.len + 1 > GUI_SELECT_TEXT_POOL )
            {
                s_select_cap.mark_count = 0;
                return;   /* capacity: keep what fits, selection just ends here */
            }

            gui_select_run_t* r = &s_select_cap.runs[ s_select_cap.run_count++ ];
            r->win  = seg->win;
            r->vp   = seg->vp;
            r->font = seg->font;
            r->x    = cmd->text.x;
            r->y    = cmd->text.y;
            r->off  = s_select_cap.text_used;
            r->len  = cmd->text.len;
            r->clip = s_draw.clip_table[ cmd->clip_idx ];

            memcpy( s_select_cap.text + r->off, s_draw.text_pool + cmd->text.off, r->len );
            s_select_cap.text[ r->off + r->len ] = '\0';
            s_select_cap.text_used += r->len + 1;
        }
    }

    s_select_cap.mark_count = 0;
}

/*============================================================================================*/
/* Read accessors for the selection controller (chrome/window/gui_select.c). */

u32 select_capture_serial( void ) { return s_select_cap.serial; }
u32 select_run_count     ( void ) { return s_select_cap.run_count; }

const gui_select_run_t*
select_run( u32 i )
{
    return i < s_select_cap.run_count ? &s_select_cap.runs[ i ] : NULL;
}

const char*
select_run_text( const gui_select_run_t* run )
{
    return s_select_cap.text + run->off;
}

// clang-format on
/*============================================================================================*/
