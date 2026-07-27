/*==============================================================================================

    runtime_service/gui/component/gui_comp_button.c -- button component: the press protocol.

    The simplest component, and the one that settles the SHARED output shape: state FIRST, then
    any geometry, then only the outcomes `state` does not already carry.  The button has no
    geometry beyond the caller's rect and its outcome IS state.clicked, so its result is just
    { state } -- the floor of the convention the slider's { state, frac, fill, handle, changed }
    extends.  (An outcome field that merely re-spells a state bit is not the shape; a caller
    reads b.state.clicked, the same bit every other component reports it in.)

    Because the whole widget is one (id, rect) -> state, there is NO desc struct: the button takes
    plain arguments (the desc is the slider's answer to being parameter-rich, not a mandate).  It
    also draws the line the pre-component fused core blurred -- IDENTITY vs LABEL: the component
    takes a pure id; the displayed label is the render's business (gui_stock_button passes the
    label as both, preserving the "##hidden" id grammar).

    Pure logic, no paint, no retained state: gui_item runs the ITEM_BUTTON protocol (hover,
    press-capture, click, keyboard nav, and the ambient GUI_ITEM_BUTTON_REPEAT clock) over the
    rect and reports the state as plain flags -- the same machine every stock and chrome button
    passes through.

==============================================================================================*/

// clang-format off

/* One press button over a caller rect: run the standard item protocol, report state + click.
   A click is a host state change this frame's screen cannot show yet, so the component asks for
   the next emit itself (else the retained cache replays the stale screen until the mouse moves). */
gui_comp_button_t
gui_comp_button( const char* id_str, gui_rect_t rect )
{
    gui_comp_button_t out = ( gui_comp_button_t ){ 0 };

    out.state = gui_item( id_str, rect );   /* ITEM_BUTTON: hover / press-capture / click / nav / repeat */

    if ( out.state.clicked )
        gui_request_redraw();
    return out;
}

// clang-format on
/*============================================================================================*/
