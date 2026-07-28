/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_tree.c -- Folding widgets.

    collapsing_header (a framed clickable bar that folds a section) and tree_node / tree_pop
    (its unframed sibling, indenting while open) -- the fold-a-block rows behind inspectors,
    outlines, and file explorers.  Open state persists per id in the keyed state pool
    (core/gui_state.c), the same store windows and combos use.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    collapsing_header -- a full-width clickable bar with a fold arrow that toggles a section open
    or closed, returning the open state.  There is no end call: the caller guards its body with the
    return ( if ( header(...) ) { widgets } ), exactly like window_begin's collapse, so a closed
    header simply skips emitting its contents.  The open flag persists across frames in the keyed
    state pool, keyed by the header id -- the same store windows, tree nodes, and combos use; this
    is the smallest example of it.  Closed by default (zeroed on first sight). */

typedef struct { bool open; } gui_header_state_t;

bool
gui_collapsing_header( const char* label )
{
    gui_id_t   id = item_id( label );
    gui_rect_t r  = cell_next( WIDGET_H );

    gui_header_state_t* hs = GUI_STATE( gui_header_state_t, id );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
    if ( st.clicked ) hs->open = !hs->open;

    /* Clickable bar with hover/active feedback, an arrow box on the left, then the label. */
    draw_face_item( r, st );

    gui_rect_t arrow = { r.x, r.y, r.h, r.h };          /* a square the height of the bar */
    draw_collapse_arrow( arrow, !hs->open, COL_TEXT_IDLE );    /* closed -> points right */
    draw_label( r.x + r.h, text_center_y( r.y, r.h ), COL_TEXT_IDLE, label );

    return hs->open;
}

/*==============================================================================================
    tree_node / tree_pop -- a collapsing_header without the frame: an arrow + label row that folds
    a nested block and indents it while open.  The unframed sibling of collapsing_header (no filled
    bar; it highlights only on hover, so a tree reads as rows rather than stacked headers) and the
    building block for file explorers / outline views.  Guard the body with the return and, when it
    is true, close it with tree_pop -- which removes exactly the indent the open node added:

        if ( gui()->tree_node( "Parent" ) )
        {
            gui()->text( "Child" );
            if ( gui()->tree_node( "Nested" ) ) { gui()->text( "Deep" ); gui()->tree_pop(); }
            gui()->tree_pop();
        }

    Open state persists per id in the keyed pool, like collapsing_header.  The indent step is one
    row height, so a child's content lines up under the parent label just past the fold arrow. */

bool
gui_tree_node( const char* label )
{
    gui_id_t   id = item_id( label );
    gui_rect_t r  = cell_next( WIDGET_H );

    gui_header_state_t* hs = GUI_STATE( gui_header_state_t, id );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
    if ( st.clicked ) hs->open = !hs->open;

    /* No framed bar: tint only on hover / active / nav (like selectable), so a tree is a list of rows. */
    if ( st.hover || st.active || st.nav )
        draw_face_item( r, st );

    gui_rect_t arrow = { r.x, r.y, r.h, r.h };          /* fold arrow in a square at the left */
    draw_collapse_arrow( arrow, !hs->open, COL_TEXT_IDLE );    /* closed -> points right */

    f32 label_x = r.x + r.h;
    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT_IDLE, label, ( r.x + r.w ) - label_x );
    cell_reach( label_x + label_width( label ) );   /* natural width may exceed the row */

    /* Indent the body while open; tree_pop removes the matching step.  Done here so children land
       inset the instant the caller starts emitting them under the true return. */
    if ( hs->open )
        gui_indent( WIDGET_H );

    return hs->open;
}

void
gui_tree_pop( void )
{
    gui_unindent( WIDGET_H );
}

// clang-format on
/*============================================================================================*/
