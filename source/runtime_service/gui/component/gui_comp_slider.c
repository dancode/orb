/*==============================================================================================

    runtime_service/gui/component/gui_comp_slider.c -- slider component: value <-> track logic.

    The reference component: the richest logic-vs-look seam in the kit, and the template every
    other component copies.  Consumes (id, rect, value, range) -- or the full desc, which adds
    snap step / handle width / nav step -- and reports the interaction state, the resolved
    fraction, and the two rects a render needs (the value BAR and the HANDLE), with NO paint.
    A stock render (gui_stock_slider) and a user's own slider both drive this and differ only in
    their draw_* calls.

    What the model pivots on:
      - MAPPING.  This is the ABSOLUTE-position slider: the handle CENTER tracks the cursor, so
        the value and the knob never disagree (resolving the pre-component core's mismatch, where
        the cursor mapped over the full width but the knob over width - handle).  The other
        pivot -- RELATIVE drag (value by cursor displacement x speed, no track, needs a press
        anchor) -- is a sibling component, not this one.
      - RANGE [lo,hi] + SNAP (step, value units): the value grid the handle lands on.
      - Handle width: an INPUT (geometry), because it defines the travel and the exact mapping;
        the handle's LOOK stays with the render.

    No retained state: the value is the caller's *v and the position is derived every frame, so
    unlike a relative drag this component keeps no per-widget anchor.

==============================================================================================*/

// clang-format off

/* One value slider over a caller rect: run behavior, map drag/nav to the value, snap + clamp,
   then hand back the fraction and the bar/handle rects.  True change is reported via .changed;
   the component requests the next frame's redraw itself (it knows the value moved).

   The _ex form takes the full desc; gui_comp_slider below is the positional common case, so
   the component tier opens ( id, rect, ... ) uniformly like every other comp_*. */

gui_comp_slider_t
gui_comp_slider_ex( const gui_comp_slider_desc_t* d )
{
    gui_comp_slider_t out = ( gui_comp_slider_t ){ 0 };

    gui_rect_t r  = d->rect;
    f32        lo = d->lo, hi = d->hi;
    f32*       v  = d->v;

    f32 handle_w = ( d->handle_w > 0.0f ) ? d->handle_w : 8.0f;   /* default reference width */
    if ( handle_w > r.w ) handle_w = r.w;
    f32 travel = r.w - handle_w;                                   /* the handle's left-edge sweep */

    gui_item_state_t st = gui_item( d->id, r );
    out.state = st;

    f32 old = *v;

    /* Captured drag: map the cursor to the handle CENTER so grabbing the knob keeps value and
       knob locked together across the whole track. */
    if ( st.active && travel > 0.0f )
    {
        f32 mx, my;
        gui_get_mouse_pos( &mx, &my );
        f32 t = ( mx - ( r.x + handle_w * 0.5f ) ) / travel;
        t  = ( t < 0.0f ) ? 0.0f : ( t > 1.0f ) ? 1.0f : t;
        *v = lo + t * ( hi - lo );
    }

    /* Keyboard value edit (this slider captured nav): one step per Left/Right repeat.  Auto step
       is the snap grid when set, else 5% of the range. */
    if ( st.nav_adjust )
    {
        f32 nav_step = ( d->nav_step > 0.0f ) ? d->nav_step
                     : ( d->step     > 0.0f ) ? d->step
                                              : ( hi - lo ) * 0.05f;
        *v += (f32)st.nav_adjust * nav_step;
    }

    /* Snap to the value grid (nearest multiple from lo), then clamp to the range. */
    if ( d->step > 0.0f )
        *v = lo + floorf( ( *v - lo ) / d->step + 0.5f ) * d->step;
    *v = ( *v < lo ) ? lo : ( *v > hi ) ? hi : *v;

    /* Fraction is authoritative FROM the settled value -- geometry follows value, not the cursor. */
    f32 frac = ( hi > lo ) ? ( *v - lo ) / ( hi - lo ) : 0.0f;
    out.frac   = frac;
    out.handle = ( gui_rect_t ){ r.x + frac * travel,       r.y, handle_w,                    r.h };
    out.fill   = ( gui_rect_t ){ r.x, r.y, frac * travel + handle_w * 0.5f, r.h };

    out.changed = ( *v != old );
    if ( out.changed )
        gui_request_redraw();
    return out;
}

/* The positional form: continuous value over [lo,hi] with the default handle width and the auto
   nav step (5% of the range).  Everything the desc adds -- a snap grid, a specific handle extent,
   an explicit keyboard step -- is what _ex is for. */
gui_comp_slider_t
gui_comp_slider( const char* id, gui_rect_t rect, f32* v, f32 lo, f32 hi )
{
    return gui_comp_slider_ex( &( gui_comp_slider_desc_t ){
        .id = id, .rect = rect, .v = v, .lo = lo, .hi = hi } );
}

// clang-format on
/*============================================================================================*/
