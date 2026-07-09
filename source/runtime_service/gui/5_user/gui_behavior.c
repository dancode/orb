/*==============================================================================================

    runtime_service/gui/5_user/gui_behavior.c -- Public interaction behavior on caller rects.

    The behavior half of the user-UI substrate: gui_item() runs the shared widget interaction
    state machine (widget_behavior, 2_interact/gui_item.c) over a rect the CALLER derived -- a
    canvas() cut, a split/carve panel, custom layout math -- and reports the resolved state as
    plain flags.  This is the seam a user widget is built on: get a rect, ask for behavior, draw
    your own presentation.  The stock widgets (3_widgets/) resolve their rects through the composer
    and pass through the same state machine, so a custom item hovers, press-captures, clicks, and
    registers for keyboard nav exactly like a built-in one -- including the modal-while-dragging
    freeze and the last-item queries (is_item_hovered, popup_context_item_begin).

    invisible_button is gui_item() reduced to its click bit, kept as the one-liner convenience.

    Included by gui.c in the 5_user/ tier (last of the tiers -- pure vocabulary, no state); needs
    widget_id (2_present/gui_widget_core.c) and widget_behavior (2_interact/gui_item.c), in scope far
    above.

==============================================================================================*/
// clang-format off

gui_item_state_t
gui_item( const char* id_str, gui_rect_t r )
{
    gui_id_t       id = widget_id( id_str );
    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    return ( gui_item_state_t ){ .hover   = st.hover,
                                 .active  = st.active,
                                 .pressed = st.pressed,
                                 .clicked = st.clicked };
}

bool
gui_invisible_button( const char* id_str, gui_rect_t r )
{
    return gui_item( id_str, r ).clicked;
}

// clang-format on
/*============================================================================================*/
