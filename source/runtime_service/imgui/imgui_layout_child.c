/*==============================================================================================

    runtime_service/imgui/imgui_layout_child.c -- Child box and sub-layout lifecycle.

    begin_child / end_child open a nested scrollable region inside the current layout: they
    carve a box from the parent pen, draw its frame, and hand off to layout_push/pop_region.
    CHILD_RESIZE_X / _Y add a draggable grip on the right / bottom edge so the box is
    user-resizable; the size is persisted in the region pool across frames.

    set_next_window_size_constraints latches a one-shot [min,max] box consumed by the next
    begin_child: an auto-sized box grows with its content only up to max_h, and a resize drag
    cannot leave the range.

    push_layout / pop_layout open a transient sub-layout inside one cell of the parent template:
    no scroll, no clip, no persistent state, no frame.  It is the recursive completion of the
    cell model -- a cell can host a layout, the way a window or child does.

    Included by imgui.c after imgui_layout_region.c (provides layout_push/pop_region,
    region_get, scroll_clamp) and imgui_resize.c (provides window_resize_hit, resize_apply_edges,
    resize_grab, window_draw_resize_highlight, IMGUI_RESIZE_*, s_resize_edges).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    begin_child / end_child -- a nested scrollable region inside the current layout.
----------------------------------------------------------------------------------------------*/

/* Smallest a resizeable child may be dragged to: a couple of rows wide, one row plus border tall. */
#define CHILD_MIN_W ( WIDGET_H * 3.0f )
#define CHILD_MIN_H ( WIDGET_H + WIN_BORDER )

/* Next-child size constraints (the Dear ImGui SetNextWindowSizeConstraints analogue): a one-shot
   [min,max] box consumed by the next begin_child.  Set by imgui_set_next_window_size_constraints,
   cleared on consume so it targets exactly one child.  A bound <= 0 means unconstrained on that
   side; the absolute CHILD_MIN floors still apply to a resize drag. */
static struct
{
    bool has;
    f32  min_w, min_h, max_w, max_h;

} s_next_child_con;

void
imgui_set_next_window_size_constraints( f32 min_w, f32 min_h, f32 max_w, f32 max_h )
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
imgui_begin_child( const char* id_str, f32 w, f32 h, imgui_win_flags_t flags )
{
    layout_frame_t* parent = lf();

    /* Combine against the active id scope (the parent region, plus any push_id) so the same child
       label nests safely under different parents and never collides with a window id. */
    imgui_id_t id = id_combine( id_seed(), id_hash( id_str ) );

    /* Persistent state (scroll offset, last-measured content extent, user-resized size), keyed by
       id.  Fetched up front: an auto-sized (h <= 0) child reads content_h, a resizeable one reads
       the user size, to fix the box. */
    imgui_region_t* rg = region_get( id );

    /* Resize is a flow-child affordance: a grid cell sizes its own child, so the flags are inert
       there.  resize_id is the active_id this child holds while its border is being dragged. */
    bool       resize_x  = ( flags & IMGUI_WIN_CHILD_RESIZE_X ) && parent->lay_nrows == 0;
    bool       resize_y  = ( flags & IMGUI_WIN_CHILD_RESIZE_Y ) && parent->lay_nrows == 0;
    imgui_id_t resize_id = id_combine( id, IMGUI_RESIZE_SALT );

    /* Consume any next-child size constraints up front (cleared so they bind only this child).  A
       grid cell sizes its own child, so the bounds, like the resize flags, are inert there; in flow
       they clamp the resolved size below and the resize-drag apply that follows. */
    f32 con_min_w = 0.0f, con_min_h = 0.0f, con_max_w = 0.0f, con_max_h = 0.0f;
    if ( s_next_child_con.has )
    {
        con_min_w = s_next_child_con.min_w;  con_min_h = s_next_child_con.min_h;
        con_max_w = s_next_child_con.max_w;  con_max_h = s_next_child_con.max_h;
        s_next_child_con.has = false;
    }

    /* Where the child box lands: in a grid parent it takes the next cell (w / h ignored -- the
       cell sizes it, the natural way to drop a scroll region into a split pane); in flow it sits
       at the pen, on its own line.  The parent pen / grid cursor is advanced by layout_pop_region
       for flow, but the grid cursor must step here since pop does not touch (col,row). */
    imgui_rect_t box;
    if ( parent->lay_nrows > 0 )
    {
        box = grid_next_rect( parent );   /* the next matrix cell; advances the matrix cursor */
    }
    else
    {
        layout_row_break( parent );       /* a flow child starts on its own line */
        if ( w <= 0.0f ) w = parent->content_w;   /* default: fill the remaining content width */

        /* A resizeable axis takes its size from the persisted user value, seeded once from the
           incoming w/h (a sensible 8-row default when h <= 0 -- RESIZE_Y supersedes auto-size). */
        if ( resize_x )
        {
            if ( rg->user_w <= 0 ) rg->user_w = (i16)w;
            w = (f32)rg->user_w;
        }
        if ( resize_y )
        {
            if ( rg->user_h <= 0 ) rg->user_h = (i16)( ( h > 0.0f ) ? h : WIDGET_H * 8.0f );
            h = (f32)rg->user_h;
        }
        /* h <= 0 (and not RESIZE_Y) auto-sizes the height to the content measured last frame (the
           AutoResizeY case): the box hugs its widgets, like an ALWAYS_AUTOSIZE window on the
           vertical axis.  Before any content is measured (first frame) it opens one widget-row
           tall and settles next frame.  An auto-sized child has nothing to scroll. */
        else if ( h <= 0.0f )
            h = ( rg->content_h > 0.0f ) ? rg->content_h + WIDGET_GAP + WIN_BORDER : WIDGET_H;

        /* Bound the resolved size by any next-child constraints: an auto-sized box hugs content up
           to max_h then the default vertical scrollbar takes over, and never shrinks below min_h. */
        w = child_con_clamp( w, con_min_w, con_max_w );
        h = child_con_clamp( h, con_min_h, con_max_h );

        box = ( imgui_rect_t ){ parent->content_x, parent->cursor_y, w, h };
    }

    /* Edge-resize interaction, resolved here -- before the body widgets -- so a press on the grip
       band pre-empts a body widget under it, mirroring how begin_window resolves the window edge
       first.  Gated on the owning window being front-most and a free or self-owned active_id.
       Apply an in-flight drag to the persisted size, then re-derive the box so the frame painted
       below tracks the cursor this frame (the right/bottom edges only -- the origin stays put). */
    u8 resize_hot = 0;
    if ( ( resize_x || resize_y ) && s_build.win_id == s_interaction.hover_win
         && ( s_interaction.active_id == IMGUI_ID_NONE || s_interaction.active_id == resize_id ) )
    {
        if ( s_interaction.active_id == resize_id )
        {
            /* Shared raw edge-drag (R / B only -- the child's top-left is pinned); the child then
               layers its own policy: clamp to the next-child constraints and the CHILD_MIN floor,
               persist into the region record, and feed the result back into the box drawn below. */
            imgui_rect_t rr = box;
            resize_apply_edges( &rr, (u8)( s_resize_edges & ( IMGUI_RESIZE_R | IMGUI_RESIZE_B ) ) );

            if ( s_resize_edges & IMGUI_RESIZE_R )
            {
                rg->user_w = (i16)child_con_clamp( rr.w, con_min_w, con_max_w );
                if ( rg->user_w < CHILD_MIN_W ) rg->user_w = (i16)CHILD_MIN_W;
                box.w = (f32)rg->user_w;
            }
            if ( s_resize_edges & IMGUI_RESIZE_B )
            {
                rg->user_h = (i16)child_con_clamp( rr.h, con_min_h, con_max_h );
                if ( rg->user_h < CHILD_MIN_H ) rg->user_h = (i16)CHILD_MIN_H;
                box.h = (f32)rg->user_h;
            }
        }

        /* Hot edges under the cursor, narrowed to this child's resizeable axes -- and only the
           grow-from-origin pair (right + bottom), since the child's top-left is pinned. */
        u8 allow   = (u8)( ( resize_x ? IMGUI_RESIZE_R : 0u ) | ( resize_y ? IMGUI_RESIZE_B : 0u ) );
        resize_hot = (u8)( window_resize_hit( box, false ) & allow );

        /* Grab on press: the shared resize_grab claims the resize active_id and records the offset
           that keeps the grabbed edge under the cursor (so the size does not jump by the band width
           at grab time).  resize_hot is only ever R / B here, so its far-edge pins go unused. */
        if ( resize_hot && s_interaction.active_id == IMGUI_ID_NONE && s_io.mouse_pressed[ 0 ] )
            resize_grab( id, box, resize_hot );

        /* Directional hardware cursor over a hot grip / during the drag (R/B only for a child). */
        u8 ce = ( s_interaction.active_id == resize_id )
              ? (u8)( s_resize_edges & ( IMGUI_RESIZE_R | IMGUI_RESIZE_B ) ) : resize_hot;
        if ( ce )
            set_mouse_cursor( resize_cursor_for_edges( ce ) );
    }

    /* The child box is chrome, not an item: paint its frame opaque even if a disabled widget
       precedes the begin_child call. */
    item_flags_chrome_reset();

    /* Child body fill, drawn under the parent clip before the region clips in.  The border is
       deferred to end_child (after the scrollbars) so the bar tracks cannot overdraw it -- the
       same deferral end_window uses for the window frame. */
    draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0,0,1,1, 0, COL_CHILD_BG );

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags,
                        &rg->scroll_x, &rg->scroll_y, &rg->content_w, &rg->content_h,
                        /* own_clip */ true );   /* the child's own scissor -- second clip in the window */

    /* Stamp the child's resize bookkeeping on its just-pushed frame, and suppress body-widget hover
       under a hot/armed edge for the child's duration (the edges stay armed mid-drag even if the
       cursor drifts off).  end_child restores the saved hot, so siblings below are unaffected. */
    layout_frame_t* f         = lf();
    f->child_resize_edge      = ( s_interaction.active_id == resize_id ) ? s_resize_edges : resize_hot;
    f->child_resize_saved_hot = s_build.win_resize_hot;
    if ( f->child_resize_edge ) s_build.win_resize_hot = f->child_resize_edge;

    /* No collapse concept for a child: always returns true, always pair with end_child. */
    return true;
}

void
imgui_end_child( void )
{
    /* Capture the box + resize state before layout_pop_region unwinds the frame, then draw the
       border after it has painted the scrollbars.  The bar tracks are inset by WIN_BORDER and butt
       against the frame, so drawing the outline last keeps it solid where a track meets the box
       edge.  The region clip is already popped, so this paints under the parent clip. */
    layout_frame_t* f     = lf();
    imgui_rect_t    box   = f->outer;
    u8              edges = f->child_resize_edge;
    u8              saved = f->child_resize_saved_hot;

    layout_pop_region();

    s_build.win_resize_hot = saved;   /* lift the body-widget suppression this child raised */

    draw_push_rect_outline( box.x, box.y, box.w, box.h, WIN_BORDER, 0, COL_BORDER );

    /* Resize affordance: bold the hot/armed edge so the border reads as draggable. */
    if ( edges )
        window_draw_resize_highlight( box, edges );
}

/*----------------------------------------------------------------------------------------------
    push_layout / pop_layout -- a sub-layout that fills one cell.

    Consumes the next cell of the active template exactly as a widget would -- so the parent
    advances (column, row wrap, same_line anchor) the instant push_layout is called, and on pop it
    resumes at the following cell -- then opens a transient layout frame whose content area *is*
    that cell.  Inside, shape it with the normal verbs (row / row_cols / grid / widgets); a fresh
    sub-layout opens undeclared, so name its mode inside (stack / columns / ...).

    A sub-layout obeys the same sizing rules as any widget: it gets one standard-height cell unless
    the row height was declared larger up front.  It does not grow the parent row to fit its
    contents -- fitting them inside the cell is the caller's job, and overflow is not clipped.
    Always pair with pop_layout, like push_id / pop_id.

    Id scope is left unchanged, so a widget inside the sub-layout shares the parent region's ids;
    use push_id / "##" to disambiguate repeats, exactly as anywhere else.
----------------------------------------------------------------------------------------------*/

/* Sink for a sub-layout's unused scroll / content-measure fields -- it never scrolls and its extent
   feeds nothing back, so these only ever hold zero / discard.  Shared by every push_layout frame. */
static f32 s_sublayout_sink[ 4 ];

void
imgui_push_layout( void )
{
    /* Take the next cell on the parent template -- this advances the parent like any widget emit. */
    imgui_rect_t cell = widget_next_rect( WIDGET_H );

    /* Cap the write slot at the top of the stack (mirroring layout_push_region) so an over-deep
       nesting aliases the deepest frame rather than writing past the array; sp still counts true. */
    u32 slot = s_layout_sp < IMGUI_LAYOUT_DEPTH ? s_layout_sp : IMGUI_LAYOUT_DEPTH - 1;
    ++s_layout_sp;
    layout_frame_t* f = &s_layout_stack[ slot ];

    /* Transient frame: no scroll, no clip, no own id scope.  The unused region fields point at the
       shared sink, and parent_clip / id_restore are saved only so pop is symmetric. */
    f->region_id   = IMGUI_ID_NONE;
    f->outer       = cell;
    f->flags       = IMGUI_WIN_NOSCROLL;
    f->parent_clip = s_build.clip_rect;
    f->pushed_clip = false;
    f->id_restore  = s_id_sp;

    s_sublayout_sink[ 0 ] = s_sublayout_sink[ 1 ] = 0.0f;
    f->scroll_x   = &s_sublayout_sink[ 0 ];
    f->scroll_y   = &s_sublayout_sink[ 1 ];
    f->pcontent_w = &s_sublayout_sink[ 2 ];
    f->pcontent_h = &s_sublayout_sink[ 3 ];

    f->sb_w = f->sb_h = 0.0f;
    f->show_v = f->show_h = false;
    f->view_w = cell.w;
    f->view_h = cell.h;

    /* Content area = the cell.  content_y_max is the cell bottom, so a grid sub-layout fills it. */
    f->origin_x      = cell.x;
    f->origin_y      = cell.y;
    f->content_x     = cell.x;
    f->content_w     = cell.w;
    f->cursor_x      = cell.x;
    f->cursor_y      = cell.y;
    f->content_max_x = cell.x;
    f->content_y_max = cell.y + cell.h;

    layout_clear( f );                       /* sub-layout opens undeclared -- declare a mode inside */
}

void
imgui_pop_layout( void )
{
    layout_frame_t* f = lf();
    s_id_sp         = f->id_restore;         /* unwind any push_id the body left open */
    s_build.clip_rect = f->parent_clip;      /* unchanged, but symmetric with push */
    if ( s_layout_sp ) --s_layout_sp;        /* parent already advanced at push -- nothing more */
}

// clang-format on
/*============================================================================================*/
