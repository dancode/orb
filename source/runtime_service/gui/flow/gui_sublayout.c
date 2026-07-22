/*==============================================================================================

    runtime_service/gui/flow/gui_sublayout.c -- Transient sub-layout lifecycle.

    push_layout / pop_layout open a transient sub-layout inside one cell of the parent template:
    no scroll, no clip, no persistent state, no frame.  It is the recursive completion of the
    cell model -- a cell can host a layout, the way a window or child does.

    sublayout_open is the shared primitive behind push_layout, push_layout_overlay, and the
    split-panel pusher in gui_split.c (included just after this file) -- each opens a transient
    frame over a caller-resolved rect, differing only in how that rect was obtained.

    Included by gui.c right after gui_layout_child.c.

==============================================================================================*/
// clang-format off

/*==============================================================================================
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
==============================================================================================*/

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
    f->view   = cell;   /* the whole cell is visible: no border, no gutters */

    /* Content area = the whole cell (no pad, no scroll bias through the zeroed sink); opens
       undeclared -- declare a mode inside.  band_bottom lands on the cell bottom, so a grid
       sub-layout fills it. */
    layout_seed_content( f, ( gui_pad_t ){ 0 } );
}

void
gui_push_layout( void )
{
    /* Take the next cell on the parent template -- this advances the parent like any widget emit. */
    gui_rect_t cell = cell_next( WIDGET_H );
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

/*==============================================================================================
    flow_begin / flow_cell / flow_end -- the named rect <-> flow seam pair.

    The blessed crossing verbs of the layer stack: flow_begin opens
    the layout engine inside ANY caller rect, however it was produced (cut_* algebra, split,
    carve, anchor, a flow cell, custom math); flow_cell takes the next flow element back out AS
    a rect; flow_end resumes the outer producer.  Thin names over the proven machinery --
    flow_begin is sublayout_open (push_layout_overlay's body), flow_cell is the shared per-item
    cell seam with 0-means-natural sizing, flow_end is pop_layout -- so the pair nests to the
    layout stack depth and the recursive contract carve -> flow -> cell -> carve -> flow holds
    at every level.  Flow never scrolls: a carved area that needs scroll / clip / persistence
    opens a core surface first (region_begin) and flows inside it.
==============================================================================================*/

void
gui_flow_begin( gui_rect_t rect )
{
    sublayout_open( rect );
}

/* The next flow element as a rect.  w / h <= 0 mean natural: the track width the template
   resolves (same as any widget) and one standard row height.  Advances the pen like a widget. */
gui_rect_t
gui_flow_cell( f32 w, f32 h )
{
    return cell_next_w( w > 0.0f ? w : -1.0f, h > 0.0f ? h : WIDGET_H );
}

void
gui_flow_end( void )
{
    gui_pop_layout();
}

// clang-format on
/*============================================================================================*/
