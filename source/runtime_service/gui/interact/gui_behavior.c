/*==============================================================================================

    runtime_service/gui/interact/gui_behavior.c -- Public interaction behavior on caller rects.

    The behavior half of the user-UI substrate: gui_item() runs the shared widget interaction
    state machine (item_state, core/gui_item.c) over a rect the CALLER derived -- a
    canvas() cut, a split/carve panel, custom layout math -- and reports the resolved state as
    plain flags.  This is the seam a user widget is built on: get a rect, ask for behavior, draw
    your own presentation.  The stock widgets (widgets/) resolve their rects through the composer
    and pass through the same state machine, so a custom item hovers, press-captures, clicks, and
    registers for keyboard nav exactly like a built-in one -- including the modal-while-dragging
    freeze and the last-item queries (is_item_hovered, popup_context_item_begin).

    invisible_button is gui_item() reduced to its click bit, kept as the one-liner convenience.

    Included by gui_interact.c LAST (pure vocabulary, no state); item_id and item_state are
    interact-server seams (core/gui_id.c / core/gui_item.c, via core/gui_core.h).

==============================================================================================*/
// clang-format off

gui_item_state_t
gui_item( const char* id_str, gui_rect_t r )
{
    /* item_state's result IS the public state record -- no translation layer. */
    return item_state( item_id( id_str ), r, ITEM_BUTTON );
}

bool
gui_invisible_button( const char* id_str, gui_rect_t r )
{
    return gui_item( id_str, r ).clicked;
}

// clang-format on
/*============================================================================================*/
