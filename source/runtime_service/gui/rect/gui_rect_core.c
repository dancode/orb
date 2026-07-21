/*==============================================================================================

    runtime_service/gui/rect/gui_rect_core.c -- the rect kit's compiled half.

    Color blend + alignment placement: the non-inline pure primitives declared at the bottom
    of rect/gui_rect.h.  Pure means pure -- no ambient state, no draw calls, no gui context;
    everything here is a function of its arguments.  Moved out of present/gui_paint_core.c
    when the rect unit was carved (GUI_SERVER_PLAN.md R1).

==============================================================================================*/

// clang-format off

/* Linear blend between two ABGR colors at t in [0,1] (0 = ca, 1 = cb).  Used by animated
   widgets that blend between palette entries rather than switching them discretely. */
u32
col_lerp( u32 ca, u32 cb, f32 t )
{
    if ( t <= 0.0f ) return ca;
    if ( t >= 1.0f ) return cb;
    f32 r0 = (f32)( ( ca       ) & 0xFF );  f32 r1 = (f32)( ( cb       ) & 0xFF );
    f32 g0 = (f32)( ( ca >>  8 ) & 0xFF );  f32 g1 = (f32)( ( cb >>  8 ) & 0xFF );
    f32 b0 = (f32)( ( ca >> 16 ) & 0xFF );  f32 b1 = (f32)( ( cb >> 16 ) & 0xFF );
    f32 a0 = (f32)( ( ca >> 24 ) & 0xFF );  f32 a1 = (f32)( ( cb >> 24 ) & 0xFF );
    u32 r  = (u32)( r0 + ( r1 - r0 ) * t );
    u32 g  = (u32)( g0 + ( g1 - g0 ) * t );
    u32 b  = (u32)( b0 + ( b1 - b0 ) * t );
    u32 a  = (u32)( a0 + ( a1 - a0 ) * t );
    return r | ( g << 8 ) | ( b << 16 ) | ( a << 24 );
}

/* Place an extent `len` within the span [org, org+avail) along one axis: centered, against the far
   edge, or (default) the near edge.  The one axis primitive every aligned placement resolves
   through -- rect_align below for a box, and draw_text_in (gui_text.c) per line of a text
   block -- so a centered label, a right-flushed caption, and a bottom-anchored run share one rule. */

static f32
align_span( f32 org, f32 avail, f32 len, bool center, bool far )
{
    if ( center ) return org + ( avail - len ) * 0.5f;
    if ( far )    return org +   avail - len;
    return org;                                                   /* near edge -- LEFT / TOP default */
}

/* Horizontal / vertical placement within a cell span, reading the matching gui_align_t bits. */
f32 align_x( f32 x, f32 w, f32 len, u32 a ) { return align_span( x, w, len, a & GUI_ALIGN_HCENTER, a & GUI_ALIGN_RIGHT  ); }
f32 align_y( f32 y, f32 h, f32 len, u32 a ) { return align_span( y, h, len, a & GUI_ALIGN_VCENTER, a & GUI_ALIGN_BOTTOM ); }

/* Place a natural nat_w x nat_h box inside `cell` per the alignment flags (gui_align_t).  The
   single seam for positioning sub-cell content -- a button's label, a checkbox box, an aligned
   text run, a separator line -- so every widget edges / centers content the same way and a
   region's align setting flows through one place.  Returns the placed rect (w/h are nat_*).
   Thin alias for the inline gui_rect_align so widgets and callers share one rule. */

gui_rect_t
rect_align( gui_rect_t cell, f32 nat_w, f32 nat_h, u32 align )
{
    return gui_rect_align( cell, nat_w, nat_h, ( gui_align_t )align );
}

// clang-format on
/*============================================================================================*/
