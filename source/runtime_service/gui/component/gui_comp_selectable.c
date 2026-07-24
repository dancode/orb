/*==============================================================================================

    runtime_service/gui/component/gui_comp_selectable.c -- list-row press component.

    A selectable is a button with one extra behavior: a click toggles an optional caller flag.
    So this composes gui_comp_button (the whole press protocol + redraw-on-click) and adds only
    the toggle -- the smallest possible new component, and a second proof that comp_button is the
    shared press core.  The row highlight (hover / selected tint) is the render's: it reads the
    caller's *selected for the active state.  selected NULL = a click-only row.

==============================================================================================*/

// clang-format off

/* One selectable row over a caller rect: run the button protocol, toggle *selected on click.
   comp_button already asked for the next emit (a click drives a caller selection not visible
   until then), so nothing more is needed here. */
gui_comp_selectable_t
gui_comp_selectable( const char* id, gui_rect_t rect, bool* selected )
{
    gui_comp_button_t b = gui_comp_button( id, rect );

    if ( b.clicked && selected )
        *selected = !*selected;

    return ( gui_comp_selectable_t ){ .state = b.state, .clicked = b.clicked };
}

// clang-format on
/*============================================================================================*/
