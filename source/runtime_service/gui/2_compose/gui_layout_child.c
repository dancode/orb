/*==============================================================================================

    runtime_service/gui/2_compose/gui_layout_child.c -- Child box and sub-layout lifecycle.

    child_begin / child_end open a nested scrollable region inside the current layout: they
    carve a box from the parent pen, draw its frame, and hand off to layout_push/pop_region.
    CHILD_RESIZE_X / _Y add a draggable grip on the right / bottom edge so the box is
    user-resizable; the size is persisted in the region pool across frames.

    window_set_next_size_constraints latches a one-shot [min,max] box consumed by the next
    child_begin: an auto-sized box grows with its content only up to max_h, and a resize drag
    cannot leave the range.

    push_layout / pop_layout open a transient sub-layout inside one cell of the parent template:
    no scroll, no clip, no persistent state, no frame.  It is the recursive completion of the
    cell model -- a cell can host a layout, the way a window or child does.

    Included by gui.c after gui_layout_region.c (provides layout_push/pop_region, region_get,
    scroll_clamp) and 2_interact/gui_resize.c (provides the resize_item protocol +
    resize_apply_edges); the chrome paint comes from 2_present/gui_widget_core.c
    (draw_child_bg / draw_child_border / draw_resize_highlight).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    child_begin / child_end -- a nested scrollable region inside the current layout.
----------------------------------------------------------------------------------------------*/

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
        if ( w <= 0.0f ) w = parent->content_w;   /* default: fill the remaining content width */

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
       resize_item service (2_interact/gui_resize.c); the child only exposes its edges and layers
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
       (draw_child_bg / draw_child_border, 2_present/gui_widget_core.c). */
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
   feeds nothing back, so this only ever holds zero / discard.  Shared by every push_layout frame. */
static gui_scroll_link_t s_sublayout_sink;

/* Open a transient sub-layout frame whose content area is `cell` (screen rect).  The shared body of
   gui_push_layout (cell = the next template cell), gui_push_layout_overlay (cell = an explicit rect),
   and the split-panel pusher: no scroll, no clip, no persistent state, no frame.  It does NOT advance
   the parent -- a caller that needs the parent to step (push_layout) reserves the cell first. */
static void
sublayout_open( gui_rect_t cell )
{
    /* Cap the write slot at the top of the stack (mirroring layout_push_region) so an over-deep
       nesting aliases the deepest frame rather than writing past the array; sp still counts true. */
    u32 slot = s_layout_sp < GUI_LAYOUT_DEPTH ? s_layout_sp : GUI_LAYOUT_DEPTH - 1;
    ++s_layout_sp;
    layout_frame_t* f = &s_layout_stack[ slot ];

    /* Transient frame: no scroll, no clip, no own id scope.  The unused region fields point at the
       shared sink, and parent_clip / id_restore are saved only so pop is symmetric. */
    f->region_id   = GUI_ID_NONE;
    f->outer       = cell;
    f->flags       = GUI_WIN_NOSCROLL;
    f->parent_clip = s_scope.clip;
    f->pushed_clip = false;
    f->id_restore  = s_id_sp;

    s_sublayout_sink.scroll_x = s_sublayout_sink.scroll_y = 0.0f;
    f->scroll = &s_sublayout_sink;

    f->sb_w = f->sb_h = 0.0f;
    f->show_v = f->show_h = false;
    f->view_w = cell.w;
    f->view_h = cell.h;

    /* Content area = the whole cell (no pad, no scroll bias through the zeroed sink); opens
       undeclared -- declare a mode inside.  band_bottom lands on the cell bottom, so a grid
       sub-layout fills it. */
    layout_seed_content( f, ( gui_pad_t ){ 0 } );
}

void
gui_push_layout( void )
{
    /* Take the next cell on the parent template -- this advances the parent like any widget emit. */
    gui_rect_t cell = widget_next_rect( WIDGET_H );
    sublayout_open( cell );
}

/* push_layout_overlay -- open a sub-layout over an explicit screen rect rather than the next template
   cell.  The parent flow is left untouched (no cell is consumed), so the rect is absolute placement:
   the seam an external layout pass (two-pass / "layout island") uses to hand a resolved box back to
   the immediate widgets, which then fill it exactly as they fill any region.  Pair with pop_layout. */
void
gui_push_layout_overlay( gui_rect_t rect )
{
    sublayout_open( rect );
}

void
gui_pop_layout( void )
{
    layout_frame_t* f = lf();
    s_id_sp         = f->id_restore;         /* unwind any push_id the body left open */
    s_scope.clip = f->parent_clip;      /* unchanged, but symmetric with push */
    if ( s_layout_sp ) --s_layout_sp;        /* parent already advanced at push -- nothing more */
}

/*----------------------------------------------------------------------------------------------
    split_begin / split_next / split_end -- two panels side by side, sharing a Y-level.

    A split is a block-level widget from the parent flow's perspective: split_begin reserves a
    rect of height max(left_h, right_h) (from the previous frame's cache) and advances the
    parent cursor past it.  Inside, each panel runs its own independent flow via the transient
    sub-layout mechanism (split_push_panel, same pattern as push_layout).

    State: gui_split_entry_t in the keyed pool records the actual content height of each side
    last frame.  First frame seeds to WIDGET_H; settles correctly on frame 2.

    Nesting: up to GUI_SPLIT_DEPTH splits may be open simultaneously (independent of the
    layout stack depth since push/pop happen within begin/next/end, not across them).
----------------------------------------------------------------------------------------------*/

#define GUI_SPLIT_DEPTH 4

typedef struct
{
    gui_id_t   id;
    gui_rect_t left_rect;
    gui_rect_t right_rect;
    f32        measured_left_h;   /* actual height emitted in left panel (recorded at split_next) */
    f32        measured_right_h;  /* actual height emitted in right panel (recorded at split_end)  */
} gui_split_frame_t;

static gui_split_frame_t s_split_stack[ GUI_SPLIT_DEPTH ];
static u32               s_split_sp;

/* Push a transient sub-layout frame whose content area is rect -- the explicit-rect sub-layout
   (sublayout_open), shared with push_layout / push_layout_overlay.  split panels never scroll. */
static void
split_push_panel( gui_rect_t rect )
{
    sublayout_open( rect );
}

/* Pop the current panel and return the content height it actually emitted: commit the open line
   and read the highwater -- under gap-before, high_y is the exact content end, so the stored
   height feeds back stably (a button_fill that fills to it reclaims the same size next frame). */
static f32
split_pop_panel( void )
{
    layout_frame_t* f = lf();
    layout_row_break( f );   /* close any partially-filled multi-column row */
    f32 h = f->high_y - f->origin_y;
    if ( h < 0.0f ) h = 0.0f;
    s_id_sp           = f->id_restore;
    s_scope.clip = f->parent_clip;
    if ( s_layout_sp ) --s_layout_sp;
    return h;
}

void
gui_split_begin( const char* id_str, f32 right_w )
{
    layout_frame_t* parent = lf();
    layout_row_break( parent );            /* close any open row before the split */

    gui_id_t           id = id_combine( id_seed(), id_hash( id_str ) );
    DBG_NAME( id, id_str );
    gui_split_entry_t* se = GUI_STATE( gui_split_entry_t, id );

    /* Resolved height: max of both sides last frame.  Seed to one row on first appearance, then
       route the target through the size-animate seam (inert today, the animation hook for later). */
    f32 target_h = se->left_h > se->right_h ? se->left_h : se->right_h;
    if ( target_h < WIDGET_H ) target_h = WIDGET_H;
    f32 resolved_h = size_animate( target_h, GUI_ID_NONE, 0.0f );

    f32 gap    = WIDGET_GAP;
    f32 left_w = parent->content_w - right_w - gap;
    if ( left_w < 0.0f ) left_w = 0.0f;

    f32 x = parent->content_x;
    f32 y = layout_next_y( parent );   /* gap-before: the split band opens below prior content */

    gui_rect_t left_rect  = { x,                y, left_w,  resolved_h };
    gui_rect_t right_rect = { x + left_w + gap, y, right_w, resolved_h };

    /* Advance the parent past the split now so it can continue below after split_end: the pen
       lands at the band's exact bottom (the gap below is owed, not appended), and line.prev_item /
       the line record are stamped so same_line() after split_end anchors to the band. */
    parent->pen_y   = y + resolved_h;   /* pen past the band */
    if ( y + resolved_h > parent->high_y )
        parent->high_y = y + resolved_h;   /* highwater climbs to the band bottom (x unchanged) */
    parent->gap_pending = true;
    parent->line.prev_item   = ( gui_rect_t ){ x, y, parent->content_w, resolved_h };
    parent->line.cross  = y;
    parent->line.ext    = resolved_h;
    parent->line.open   = false;

    /* Push the split frame and open the left panel. */
    ORB_ASSERT( s_split_sp < GUI_SPLIT_DEPTH );
    if ( s_split_sp < GUI_SPLIT_DEPTH )
    {
        gui_split_frame_t* sf = &s_split_stack[ s_split_sp++ ];
        sf->id               = id;
        sf->left_rect        = left_rect;
        sf->right_rect       = right_rect;
        sf->measured_left_h  = 0.0f;
        sf->measured_right_h = 0.0f;
        split_push_panel( left_rect );
    }
}

void
gui_split_next( void )
{
    if ( s_split_sp == 0 ) return;
    gui_split_frame_t* sf = &s_split_stack[ s_split_sp - 1 ];

    sf->measured_left_h = split_pop_panel();
    split_push_panel( sf->right_rect );
}

void
gui_split_end( void )
{
    if ( s_split_sp == 0 ) return;
    gui_split_frame_t* sf = &s_split_stack[ --s_split_sp ];

    sf->measured_right_h = split_pop_panel();

    /* Persist both heights into the keyed pool for the next frame's pre-allocation. */
    gui_split_entry_t* se = GUI_STATE( gui_split_entry_t, sf->id );
    se->left_h  = sf->measured_left_h;
    se->right_h = sf->measured_right_h;
}

// clang-format on
/*============================================================================================*/
