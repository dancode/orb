/*==============================================================================================

    runtime_service/gui/component/gui_comp_check.c -- checkbox component: toggle logic + box geom.

    A toggle over an inscribed square: the component computes the box (centered, side = the rect's
    shorter axis), runs the button protocol on it (so the box IS the hit), and flips *v on click.
    It returns the box so the render knows exactly where to draw the frame and the mark -- no
    hidden padding.  Pure logic, no paint; the label (if any) is the render's / caller's business.

==============================================================================================*/

// clang-format off

/* One checkbox over a caller rect: inscribe the box, run the press protocol on it, toggle *v.
   The click drives a caller-visible flip the current frame's screen can't show, so like every
   button the component asks for the next emit itself. */
gui_comp_check_t
gui_comp_check( const char* id, gui_rect_t rect, bool* v )
{
    gui_comp_check_t out = ( gui_comp_check_t ){ 0 };

    f32 side = ( rect.w < rect.h ) ? rect.w : rect.h;
    out.box  = gui_rect_align( rect, side, side, GUI_ALIGN_CENTER );

    out.state = gui_item( id, out.box );   /* the inscribed box IS the hit */
    if ( out.state.clicked )
    {
        *v          = !*v;
        out.changed = true;
        gui_request_redraw();
    }
    return out;
}

// clang-format on
/*============================================================================================*/
