/*==============================================================================================

    runtime_service/gui/flow/gui_layout_child.c -- Child box lifecycle.

    child_begin / child_end open a nested scrollable region inside the current layout: they
    carve a box from the parent pen, draw its frame, and hand off to layout_push/pop_region.
    CHILD_RESIZE_X / _Y add a draggable grip on the right / bottom edge so the box is
    user-resizable; the size is persisted in the region pool across frames.

    window_set_next_size_constraints latches a one-shot [min,max] box consumed by the next
    child_begin: an auto-sized box grows with its content only up to max_h, and a resize drag
    cannot leave the range.

    The transient sub-layout (push_layout/pop_layout) and the side-by-side split panel
    (split_begin/next/end) are separate features built on the same sublayout_open primitive --
    they live in gui_sublayout.c and gui_split.c, included just after this file.

    Included by gui.c after gui_scroll.c (provides layout_push/pop_region, region_get,
    scroll_clamp) and interact/gui_resize.c (provides the resize_item protocol +
    resize_apply_edges); the chrome paint comes from element/gui_adornment.c
    (draw_child_bg / draw_child_border / draw_resize_highlight).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    child_begin / child_end -- a nested scrollable region inside the current layout.
==============================================================================================*/

/* Smallest a resizeable child may be dragged to: a couple of rows wide, one row plus border tall. */
#define CHILD_MIN_W ( WIDGET_H * 3.0f )
#define CHILD_MIN_H ( WIDGET_H + WIN_BORDER )

/* Next-child size constraints (the Dear ImGui SetNextWindowSizeConstraints analogue): a one-shot
   [min,max] box consumed by the next child_begin.  Set by gui_window_set_next_size_constraints,
   cleared on consume so it targets exactly one child.  A bound <= 0 means unconstrained on that
   side; the absolute CHILD_MIN floors still apply to a resize drag. */
static struct
{
    bool has;
    f32  min_w, min_h, max_w, max_h;

} s_next_child_con;

void
gui_window_set_next_size_constraints( f32 min_w, f32 min_h, f32 max_w, f32 max_h )
{
    s_next_child_con.has   = true;
    s_next_child_con.min_w = min_w;
    s_next_child_con.min_h = min_h;
    s_next_child_con.max_w = max_w;
    s_next_child_con.max_h = max_h;
}

/* Clamp v into [mn, mx], treating a non-positive bound as unconstrained on that side. */
static f32
child_con_clamp( f32 v, f32 mn, f32 mx )
{
    if ( mn > 0.0f && v < mn ) v = mn;
    if ( mx > 0.0f && v > mx ) v = mx;
    return v;
}

bool
gui_child_begin( const char* id_str, f32 w, f32 h, gui_win_flags_t flags )
{
    layout_frame_t* parent = lf();

    /* Combine against the active id scope (the parent region, plus any push_id) so the same child
       label nests safely under different parents and never collides with a window id. */
    gui_id_t id = id_combine( id_seed(), id_hash( id_str ) );
    DBG_NAME( id, id_str );

    /* Persistent state (scroll offset, last-measured content extent, user-resized size), keyed by
       id.  Fetched up front: an auto-sized (h <= 0) child reads content_h, a resizeable one reads
       the user size, to fix the box. */
    gui_region_t* rg = region_get( id );

    /* Resize is a flow-child affordance: a grid cell sizes its own child, so the flags are inert
       there. */
    ORB_ASSERT_MSG( !( ( flags & ( GUI_WIN_CHILD_RESIZE_X | GUI_WIN_CHILD_RESIZE_Y ) )
                       && parent->tmpl.nrows > 0 ),
                    "CHILD_RESIZE_X/_Y has no effect inside a grid-cell parent" );
    bool resize_x = ( flags & GUI_WIN_CHILD_RESIZE_X ) && parent->tmpl.nrows == 0;
    bool resize_y = ( flags & GUI_WIN_CHILD_RESIZE_Y ) && parent->tmpl.nrows == 0;

    /* Consume any next-child size constraints up front (cleared so they bind only this child).  A
       grid cell sizes its own child, so the bounds, like the resize flags, are inert there; in flow
       they clamp the resolved size below and the resize-drag apply that follows. */
    f32 con_min_w = 0.0f, con_min_h = 0.0f, con_max_w = 0.0f, con_max_h = 0.0f;
    if ( s_next_child_con.has )
    {
        ORB_ASSERT_MSG( parent->tmpl.nrows == 0,
                        "window_set_next_size_constraints has no effect inside a grid-cell parent" );
        con_min_w = s_next_child_con.min_w;  con_min_h = s_next_child_con.min_h;
        con_max_w = s_next_child_con.max_w;  con_max_h = s_next_child_con.max_h;
        s_next_child_con.has = false;
    }

    /* Where the child box lands: in a grid parent it takes the next cell (w / h ignored -- the
       cell sizes it, the natural way to drop a scroll region into a split pane); in flow it sits
       at the pen, on its own line.  The parent pen / grid cursor is advanced by layout_pop_region
       for flow, but the grid cursor must step here since pop does not touch (col,row). */
    gui_rect_t box;
    if ( parent->tmpl.nrows > 0 )
    {
        box = grid_next_rect( parent, -1.0f );   /* the next matrix cell, filled; advances the matrix cursor */
    }
    else
    {
        layout_row_break( parent );       /* a flow child starts on its own line */
        if ( w <= 0.0f )
        {
            /* Default: fill the content width -- clamped to the visible track.  The content
               column can run wider than the view (an overflowing sibling widened it), and a
               child is an opaque interactive surface: defaulted to the full column it would
               seat itself under the parent's scrollbar gutter and border.  The safe fill is
               the glass, so passing 0 always "just works". */
            w = parent->content_w;
            f32 vis = parent->view.w - parent->pad.l - parent->pad.r;
            if ( w > vis ) w = vis;
        }

        /* A resizeable axis takes its size from the persisted user value, seeded once from the
           incoming w/h (a sensible 8-row default when h <= 0 -- RESIZE_Y supersedes auto-size). */
        if ( resize_x )
        {
            if ( rg->user_w <= 0.0f ) rg->user_w = w;
            w = size_animate( rg->user_w, GUI_ID_NONE, 0.0f );
        }
        if ( resize_y )
        {
            if ( rg->user_h <= 0.0f ) rg->user_h = ( h > 0.0f ) ? h : WIDGET_H * 8.0f;
            h = size_animate( rg->user_h, GUI_ID_NONE, 0.0f );
        }
        /* h <= 0 (and not RESIZE_Y) auto-sizes the height to the content measured last frame (the
           AutoResizeY case): the box hugs its widgets, like an ALWAYS_AUTOSIZE window on the
           vertical axis.  Before any content is measured (first frame) it opens one widget-row
           tall and settles next frame.  An auto-sized child has nothing to scroll. */
        else if ( h <= 0.0f )
            h = size_animate( ( rg->scroll.content_h > 0.0f ) ? rg->scroll.content_h + WIN_BORDER : WIDGET_H,
                              GUI_ID_NONE, 0.0f );

        /* Bound the resolved size by any next-child constraints: an auto-sized box hugs content up
           to max_h then the default vertical scrollbar takes over, and never shrinks below min_h. */
        w = child_con_clamp( w, con_min_w, con_max_w );
        h = child_con_clamp( h, con_min_h, con_max_h );

        box = ( gui_rect_t ){ parent->content_x, layout_next_y( parent ), w, h };
    }

    /* Edge-resize interaction, resolved here -- before the body widgets -- so a press on the grip
       band pre-empts a body widget under it, mirroring how window_begin resolves the window edge
       first.  The protocol (window gating, hot band, grab on press, directional cursor) is the
       resize_item service (interact/gui_resize.c); the child only exposes its edges and layers
       its own size policy on an in-flight drag: clamp to the next-child constraints and the
       CHILD_MIN floor, persist into the region record, and feed the result back into the box
       drawn below.  Right/bottom only -- the child's top-left is pinned. */
    u8 resize_hot = 0;
    if ( resize_x || resize_y )
    {
        u8   allow    = (u8)( ( resize_x ? GUI_RESIZE_R : 0u ) | ( resize_y ? GUI_RESIZE_B : 0u ) );
        bool dragging = false;
        resize_hot = resize_item( id, s_scope.win, box, allow, false, &dragging );

        if ( dragging )
        {
            gui_rect_t rr = box;
            resize_apply_edges( &rr, resize_hot );

            if ( resize_hot & GUI_RESIZE_R )
            {
                rg->user_w = child_con_clamp( rr.w, con_min_w, con_max_w );
                if ( rg->user_w < CHILD_MIN_W ) rg->user_w = CHILD_MIN_W;
                box.w = rg->user_w;
            }
            if ( resize_hot & GUI_RESIZE_B )
            {
                rg->user_h = child_con_clamp( rr.h, con_min_h, con_max_h );
                if ( rg->user_h < CHILD_MIN_H ) rg->user_h = CHILD_MIN_H;
                box.h = rg->user_h;
            }
        }
    }

    /* The child box is chrome, not an item: paint its frame opaque even if a disabled widget
       precedes the child_begin call. */
    item_flags_chrome_reset();

    /* Child body fill, drawn under the parent clip before the region clips in.  The border is
       deferred to child_end (after the scrollbars) so the bar tracks cannot overdraw it -- the
       same deferral window_end uses for the window frame.  Paint policy lives with the skin
       (draw_child_bg / draw_child_border, element/gui_adornment.c). */
    draw_child_bg( box );

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags, &rg->scroll,
                        /* own_clip */ !( flags & GUI_WIN_NO_CLIP ) );

    /* Stamp the child's resize bookkeeping on its just-pushed frame, and suppress body-widget hover
       under a hot/armed edge for the child's duration (the edges stay armed mid-drag even if the
       cursor drifts off).  child_end restores the saved hot, so siblings below are unaffected. */
    layout_frame_t* f         = lf();
    f->child_resize_edge      = resize_hot;   /* live edges: dragged mid-drag, else hot under cursor */
    f->child_resize_saved_hot = s_scope.resize_hot;
    if ( f->child_resize_edge ) s_scope.resize_hot = f->child_resize_edge;

    /* No collapse concept for a child: always returns true, always pair with child_end. */
    return true;
}

void
gui_child_end( void )
{
    /* Capture the box + resize state before layout_pop_region unwinds the frame, then draw the
       border after it has painted the scrollbars.  The bar tracks are inset by WIN_BORDER and butt
       against the frame, so drawing the outline last keeps it solid where a track meets the box
       edge.  The region clip is already popped, so this paints under the parent clip. */
    layout_frame_t* f     = lf();
    gui_rect_t    box   = f->outer;
    u8              edges = f->child_resize_edge;
    u8              saved = f->child_resize_saved_hot;

    layout_pop_region();

    s_scope.resize_hot = saved;   /* lift the body-widget suppression this child raised */

    draw_child_border( box );

    /* Resize affordance: bold the hot/armed edge so the border reads as draggable. */
    if ( edges )
        draw_resize_highlight( box, edges );
}

// clang-format on
/*============================================================================================*/
