/*==============================================================================================

    runtime_service/gui/component/gui_comp_cycle.c -- "< value >" stepper component.

    A COMPOUND component: two cap buttons that step *idx down / up with wrap, and a center region
    for the current value.  It composes gui_comp_button for each cap -- the proof the shared item
    core stacks up -- so each cap hovers, keyboard-navs, and redraws-on-click exactly like a
    standalone button, and this component only adds the geometry (the two caps + the center) and
    the wrap arithmetic.

    It takes `count` (to wrap the index) but NOT the item strings: the displayed value is the
    render's to draw at .label.  Pure logic, no paint.

==============================================================================================*/

// clang-format off

/* One cycle over a caller rect: cut square caps off both ends, run a comp_button on each under a
   push of the caller id (so the two caps get distinct, stable ids), and step *idx with wrap.
   comp_button already requests the redraw on a cap click, so a stepped index shows next frame. */
gui_comp_cycle_t
gui_comp_cycle( const char* id, gui_rect_t rect, i32* idx, i32 count )
{
    gui_comp_cycle_t out  = ( gui_comp_cycle_t ){ 0 };
    gui_rect_t       full = rect;

    out.prev_box = gui_rect_cut_left ( &rect, rect.h );   /* square cap, row-height wide */
    out.next_box = gui_rect_cut_right( &rect, rect.h );
    out.label    = rect;                                   /* the remaining center band */

    gui_push_id( id );
    i32 old  = *idx;
    out.prev = gui_comp_button( "##prev", out.prev_box );
    out.next = gui_comp_button( "##next", out.next_box );
    gui_pop_id();

    if ( out.prev.state.clicked && count > 0 ) *idx = ( *idx + count - 1 ) % count;
    if ( out.next.state.clicked && count > 0 ) *idx = ( *idx + 1 ) % count;

    /* The shared shape's leading field: the WHOLE stepper's interaction, so item_phase works on a
       cycle like any other component.  Hover is either cap or the value band; active / clicked
       come from whichever cap owns the gesture. */
    out.state       = out.prev.state.active ? out.prev.state
                    : out.next.state.active ? out.next.state
                                            : ( gui_item_state_t ){ 0 };
    out.state.hover = out.prev.state.hover || out.next.state.hover
                    || gui_is_mouse_hovering_rect( full );
    out.state.nav   = out.prev.state.nav || out.next.state.nav;

    out.changed = ( *idx != old );
    return out;
}

// clang-format on
/*============================================================================================*/
