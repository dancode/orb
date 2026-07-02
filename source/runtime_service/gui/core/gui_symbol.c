/*==============================================================================================

    runtime_service/gui/core/gui_symbol.c -- Symbol + shape render primitives.

    The frontend "render route" palette: the small glyph marks the chrome draws (arrows, check,
    bullet, close, pointer beak) plus the broader shape family editor / custom widgets reach for
    (frames, per-corner rounded rects, regular polygons, circles / rings / arcs / pie wedges,
    bezier curves, dashed lines, checker / hatch fills, gradients, soft shadows, outlined / shadowed
    text, resize grips, spinners, progress arcs).  This is the Dear ImGui Render* / AddXxx family.

    These compose the *backend* primitives (draw_push_triangle / _circle_filled / _rect_filled /
    _rect_outline / _text, gui_draw_line / gui_draw_polyline) into named marks -- they draw
    through the normal vertex pipeline, NOT the runtime icon atlas (gui_icon.c).  Two routes do
    the heavy lifting: a triangle fan (fill_convex) fills any convex outline, and a closed / open
    polyline strokes it; arcs are sampled from cos / sin once per call.

    Most commands carry one abgr, but GUI_CMD_RECT_GRADIENT carries two and lets the GPU's
    per-vertex color interpolation blend them, so draw_gradient is an exact one-quad blend (not
    banded).  draw_shadow's gaussian glow is still approximated with layered rings -- a future
    multi-corner-color command (or routing the rings through gradient quads) would make it exact;
    the public surface would not change.  Everything else is pixel-exact.

    Included by gui.c immediately after gui_widget_core.c, so it sees the COL_* / ROUND_* /
    WIN_BORDER macros, col_lerp, and rect_align defined there, and every widget file below resolves
    these draw_* helpers by name.  The public gui_draw_* surface over them is at the foot.

==============================================================================================*/
// clang-format off

#define SYM_PI   3.14159265358979f
#define SYM_TAU  ( 2.0f * SYM_PI )

/*----------------------------------------------------------------------------------------------
    Shared geometry helpers
----------------------------------------------------------------------------------------------*/

/* Forward decl: draw_rule (shapes section) strokes through draw_dashed_line, defined further down. */
static void draw_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 col );

/* Make a vec2 (the backend file owns its own v2; this is the UI unit's local one). */
static gui_vec2_t sv2( f32 x, f32 y ) { return ( gui_vec2_t ){ x, y }; }

/* Segment count for an arc of radius r sweeping `sweep` radians: ~one segment per 6px of arc
   length, clamped so tiny marks stay cheap (3) and big wheels stay smooth (64). */
static u32
sym_arc_segs( f32 r, f32 sweep )
{
    if ( sweep < 0.0f ) sweep = -sweep;
    u32 n = (u32)( ( r * sweep ) / 6.0f );
    if ( n < 3 )  n = 3;
    if ( n > 64 ) n = 64;
    return n;
}

/* Sample the arc (cx,cy,r) from a0 to a1 into `out` (caller-sized for segs+1 points, <= 65);
   returns the point count.  Angles are radians in screen space (y down, so +sin goes down). */
static u32
sym_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, gui_vec2_t* out )
{
    u32 segs = sym_arc_segs( r, a1 - a0 );
    for ( u32 i = 0; i <= segs; ++i )
    {
        f32 a = a0 + ( a1 - a0 ) * ( (f32)i / (f32)segs );
        out[ i ] = sv2( cx + cosf( a ) * r, cy + sinf( a ) * r );
    }
    return segs + 1;
}

/* Fill a convex outline as a triangle fan from the first point (the one fill route every convex
   shape here -- polygon, pie, per-corner rounded rect -- shares).  Each triangle is one draw
   command, so a high-segment fill is command-heavy; prefer draw_push_circle_filled for a plain
   disc, which is a single command. */
static void
fill_convex( const gui_vec2_t* pts, u32 n, u32 col )
{
    for ( u32 i = 1; i + 1 < n; ++i )
        draw_push_triangle( pts[ 0 ].x, pts[ 0 ].y, pts[ i ].x, pts[ i ].y,
                            pts[ i + 1 ].x, pts[ i + 1 ].y, 0, col );
}

/*----------------------------------------------------------------------------------------------
    Glyph marks  (the chrome's symbol set; the Dear ImGui Render* glyphs)
----------------------------------------------------------------------------------------------*/

/* Chevron glyph: a stroked '>' pointing `dir`, centered in `box` (the open sibling of the filled
   arrow).  Three points -- the two back corners and the apex -- stroked as an open polyline, sized
   to the same half-extent as draw_arrow so the two styles are interchangeable at any font size. */
static void
draw_chevron( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 color )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( box.h * 0.22f );
    if ( thickness < 1.0f ) thickness = 1.0f;

    gui_vec2_t p[ 3 ];
    switch ( dir )
    {
        case GUI_DIR_LEFT:  p[0]=sv2(cx+s,cy-s); p[1]=sv2(cx-s,cy);   p[2]=sv2(cx+s,cy+s); break;
        case GUI_DIR_RIGHT: p[0]=sv2(cx-s,cy-s); p[1]=sv2(cx+s,cy);   p[2]=sv2(cx-s,cy+s); break;
        case GUI_DIR_UP:    p[0]=sv2(cx-s,cy+s); p[1]=sv2(cx,  cy-s); p[2]=sv2(cx+s,cy+s); break;
        case GUI_DIR_DOWN:  p[0]=sv2(cx-s,cy-s); p[1]=sv2(cx,  cy+s); p[2]=sv2(cx+s,cy-s); break;
        default:              return;
    }
    gui_draw_polyline( p, 3, thickness, GUI_STROKE_CENTER, false, color );
}

/* Directional arrow glyph: a filled triangle (default) or a stroked chevron pointing `dir`,
   centered in `box`.  The one arrow generator -- arrow_button, draw_collapse_arrow, the combo /
   submenu arrow, and the dock overlay all draw through it, so the shape is uniform and follows
   GUI_VAR_ARROW_STYLE (the chevron variant) exactly as check / bullet follow their style vars.
   The half-extent scales with the box so every arrow matches the others at any font size. */
static void
draw_arrow( gui_rect_t box, gui_dir_t dir, u32 color )
{
    if ( style_var( GUI_VAR_ARROW_STYLE ) >= 0.5f )
    {
        f32 t = floorf( box.h * 0.13f );  if ( t < 1.5f ) t = 1.5f;
        draw_chevron( box, dir, t, color );
        return;
    }

    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( box.h * 0.22f );   /* triangle half-extent */

    switch ( dir )
    {
        case GUI_DIR_LEFT:  draw_push_triangle( cx - s, cy,     cx + s, cy - s, cx + s, cy + s, 0, color ); break;
        case GUI_DIR_RIGHT: draw_push_triangle( cx + s, cy,     cx - s, cy - s, cx - s, cy + s, 0, color ); break;
        case GUI_DIR_UP:    draw_push_triangle( cx,     cy - s, cx - s, cy + s, cx + s, cy + s, 0, color ); break;
        case GUI_DIR_DOWN:  draw_push_triangle( cx,     cy + s, cx - s, cy - s, cx + s, cy - s, 0, color ); break;
    }
}

/* Collapse toggle glyph: points down when expanded, right when collapsed (the following label reads
   as the thing being toggled).  Exactly the DOWN / RIGHT case of draw_arrow, so the window title bar
   and collapsing_header fold with the identical glyph arrow_button draws.  Shared by both. */
static void
draw_collapse_arrow( gui_rect_t box, bool collapsed, u32 color )
{
    draw_arrow( box, collapsed ? GUI_DIR_RIGHT : GUI_DIR_DOWN, color );
}

/* Check-mark glyph: a two-stroke 'v' fitted and centered in `box` (Dear ImGui RenderCheckMark).
   Two antialiased line segments -- a short down-stroke into the valley, then a long up-stroke --
   the same line primitive the close 'X' uses, so ticks and crosses stroke identically.  The
   geometry is expressed as fractions of the fitted square so it scales crisply at any box size. */
static void
draw_check_mark( gui_rect_t box, u32 color )
{
    f32 sz = box.w < box.h ? box.w : box.h;
    f32 ox = box.x + ( box.w - sz ) * 0.5f;     /* center the glyph square in the box */
    f32 oy = box.y + ( box.h - sz ) * 0.5f;
    f32 t  = floorf( sz * 0.15f );  if ( t < 1.5f ) t = 1.5f;   /* stroke thickness */

    /* Three points: start (upper-left), valley (lower-middle), end (upper-right). */
    f32 ax = ox + sz * 0.18f, ay = oy + sz * 0.52f;
    f32 bx = ox + sz * 0.42f, by = oy + sz * 0.74f;
    f32 cx = ox + sz * 0.82f, cy = oy + sz * 0.26f;

    gui_draw_line( ax, ay, bx, by, t, color );
    gui_draw_line( bx, by, cx, cy, t, color );
}

/* Bullet glyph: a small filled disc centered at (cx,cy) (Dear ImGui RenderBullet).  The round
   sibling of the square bullet -- the bullet widget picks between them on GUI_VAR_BULLET_STYLE. */
static void
draw_bullet( f32 cx, f32 cy, f32 r, u32 color )
{
    draw_push_circle_filled( cx, cy, r, 12, color );
}

/* Close glyph: the two-diagonal 'X' centered in `box` (Dear ImGui's CloseButton cross).  Extracted
   so the native caption close button and any other caller stroke the identical mark. */
static void
draw_close_x( gui_rect_t box, u32 color )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 m  = box.w < box.h ? box.w : box.h;
    f32 s  = floorf( m * 0.18f );   /* glyph half-extent -- matches the caption min/max glyphs */
    f32 t  = WIN_BORDER;

    gui_draw_line( cx - s, cy - s, cx + s, cy + s, t, color );
    gui_draw_line( cx - s, cy + s, cx + s, cy - s, t, color );
}

/* Arrow whose apex points AT a specific coordinate (Dear ImGui RenderArrowPointingAt): a filled
   triangle of half-extent `half` with its tip exactly on (tx,ty), opening away in `dir`.  Used for
   pointer chrome -- a tooltip / popup beak, a "jump to" marker -- where the tip must land on a point
   rather than be centered in a box (the box-centered case is draw_arrow).  Always filled. */
static void
draw_arrow_pointing_at( f32 tx, f32 ty, f32 half, gui_dir_t dir, u32 color )
{
    switch ( dir )
    {
        case GUI_DIR_LEFT:  draw_push_triangle( tx, ty, tx + half, ty - half, tx + half, ty + half, 0, color ); break;
        case GUI_DIR_RIGHT: draw_push_triangle( tx, ty, tx - half, ty - half, tx - half, ty + half, 0, color ); break;
        case GUI_DIR_UP:    draw_push_triangle( tx, ty, tx - half, ty + half, tx + half, ty + half, 0, color ); break;
        case GUI_DIR_DOWN:  draw_push_triangle( tx, ty, tx - half, ty - half, tx + half, ty - half, 0, color ); break;
    }
}

/* Plus / minus glyph: a horizontal bar, plus a vertical bar for the '+' form, centered in `box`
   (tree expand / collapse, zoom in / out).  Strokes so it tracks the arrow / chevron weight. */
static void
draw_plus_minus( gui_rect_t box, bool plus, f32 thickness, u32 color )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( ( box.w < box.h ? box.w : box.h ) * 0.3f );
    if ( thickness < 1.0f ) thickness = 1.0f;

    gui_draw_line( cx - s, cy, cx + s, cy, thickness, color );
    if ( plus )
        gui_draw_line( cx, cy - s, cx, cy + s, thickness, color );
}

/* Checkbox / menu indicator: the mark drawn inside the `box` when checked, switched on
   GUI_VAR_CHECK_STYLE -- a 'v' tick (default), a filled disc, or an 'X' cross.  The one place the
   three-way style resolves, so checkbox and menu_item stay identical and a future style lands once. */
static void
draw_check_indicator( gui_rect_t box, u32 col )
{
    u32 style = (u32)( style_var( GUI_VAR_CHECK_STYLE ) + 0.5f );
    if ( style == GUI_CHECK_DISC )
        draw_push_circle_filled( box.x + box.w * 0.5f, box.y + box.h * 0.5f,
                                 box.w * 0.5f - (f32)s_style.checkmark_pad, 16, col );
    else if ( style == GUI_CHECK_CROSS )
        draw_close_x( box, col );
    else
        draw_check_mark( box, col );
}

/* Horizontal rule centered on yc, honoring GUI_VAR_SEPARATOR_STYLE (solid fill or dashed line).
   The shared draw seam for separator() and the two rules of separator_text(). */
static void
draw_rule( f32 x, f32 yc, f32 w, f32 thickness, u32 col )
{
    if ( w <= 0.0f )
        return;
    if ( style_var( GUI_VAR_SEPARATOR_STYLE ) >= 0.5f )
        draw_dashed_line( x, yc, x + w, yc, 6.0f, 4.0f, thickness, col );
    else
        draw_push_rect_filled( x, yc - thickness * 0.5f, w, thickness, 0, 0, 1, 1, 0, col );
}

/*----------------------------------------------------------------------------------------------
    Shapes  (the convex-fill / polyline-stroke palette)
----------------------------------------------------------------------------------------------*/

/* Frame / bezel (Dear ImGui RenderFrame): a filled rounded body with an optional border, the basis
   every widget frame shares.  Uses the control-frame rounding (ROUND_WIDGET) so a custom-drawn frame
   matches the built-in buttons / inputs; pass border <= 0 to skip the outline. */
static void
draw_frame( gui_rect_t r, u32 col_bg, u32 col_border, f32 border )
{
    f32 save = draw_rounding();
    draw_set_rounding( ROUND_WIDGET );
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0, 0, 1, 1, 0, col_bg );
    if ( border > 0.0f )
        draw_push_rect_outline( r.x, r.y, r.w, r.h, border, 0, col_border );
    draw_set_rounding( save );
}

/* Build the clockwise perimeter of a per-corner rounded rect into `out` (caller-sized; <= 4*17+4).
   Each corner is a quarter arc (or a single sharp point when its radius is ~0), so a tab is two
   rounded top corners + two square bottom ones, a notch the inverse.  Radii are clamped to the
   box half-extents.  Returns the point count.  Corner arcs are capped low (UI radii are small) to
   keep a fanned fill from exploding into hundreds of triangles. */
static u32
round_rect_perimeter_ex( gui_rect_t b, f32 rtl, f32 rtr, f32 rbr, f32 rbl, gui_vec2_t* out )
{
    f32 hw = b.w * 0.5f, hh = b.h * 0.5f;
    if ( rtl > hw ) rtl = hw;  if ( rtl > hh ) rtl = hh;
    if ( rtr > hw ) rtr = hw;  if ( rtr > hh ) rtr = hh;
    if ( rbr > hw ) rbr = hw;  if ( rbr > hh ) rbr = hh;
    if ( rbl > hw ) rbl = hw;  if ( rbl > hh ) rbl = hh;

    f32 xl = b.x, xr = b.x + b.w, yt = b.y, yb = b.y + b.h;
    u32 n  = 0;

    /* Each corner: a single point when square, else a quarter arc about its inset centre.  Order is
       clockwise in screen space: top-left -> top-right -> bottom-right -> bottom-left. */
    if ( rtl < 0.5f ) out[ n++ ] = sv2( xl, yt );
    else              n += sym_arc( xl + rtl, yt + rtl, rtl, SYM_PI,        SYM_PI * 1.5f, out + n );
    if ( rtr < 0.5f ) out[ n++ ] = sv2( xr, yt );
    else              n += sym_arc( xr - rtr, yt + rtr, rtr, SYM_PI * 1.5f, SYM_TAU,       out + n );
    if ( rbr < 0.5f ) out[ n++ ] = sv2( xr, yb );
    else              n += sym_arc( xr - rbr, yb - rbr, rbr, 0.0f,          SYM_PI * 0.5f, out + n );
    if ( rbl < 0.5f ) out[ n++ ] = sv2( xl, yb );
    else              n += sym_arc( xl + rbl, yb - rbl, rbl, SYM_PI * 0.5f, SYM_PI,        out + n );
    return n;
}

/* Per-corner rounded rect, filled (triangle fan) or stroked (closed polyline).  The general path
   for tab / notch / asymmetric shapes; for a uniform radius prefer the public draw_round_rect,
   which delegates to the backend's single-command rounded rect. */
static void
draw_round_rect_ex( gui_rect_t b, f32 rtl, f32 rtr, f32 rbr, f32 rbl,
                    bool filled, f32 thickness, u32 col )
{
    gui_vec2_t pts[ 4 * 17 + 4 ];
    u32 n = round_rect_perimeter_ex( b, rtl, rtr, rbr, rbl, pts );
    if ( filled )
        fill_convex( pts, n, col );
    else
        gui_draw_polyline( pts, n, thickness < 1.0f ? 1.0f : thickness, GUI_STROKE_CENTER, true, col );
}

/* Regular n-gon centred at (cx,cy), circumradius r, first vertex at angle `rot`.  Filled (fan) or
   stroked (closed polyline) -- generalizes the triangle / diamond / hexagon marks. */
static void
draw_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col )
{
    if ( sides < 3 )  sides = 3;
    if ( sides > 64 ) sides = 64;
    gui_vec2_t pts[ 64 ];
    for ( u32 i = 0; i < sides; ++i )
    {
        f32 a = rot + SYM_TAU * ( (f32)i / (f32)sides );
        pts[ i ] = sv2( cx + cosf( a ) * r, cy + sinf( a ) * r );
    }
    if ( filled )
        fill_convex( pts, sides, col );
    else
        gui_draw_polyline( pts, sides, thickness < 1.0f ? 1.0f : thickness, GUI_STROKE_CENTER, true, col );
}

/* Circle at arbitrary radius: filled (single backend disc command) or stroked (a closed polyline
   ring of `thickness`).  The ring is the outlined form -- pass filled = false with a thickness. */
static void
draw_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col )
{
    if ( filled )
    {
        draw_push_circle_filled( cx, cy, r, sym_arc_segs( r, SYM_TAU ), col );
        return;
    }
    u32 segs = sym_arc_segs( r, SYM_TAU );
    gui_vec2_t pts[ 64 ];
    for ( u32 i = 0; i < segs; ++i )
    {
        f32 a = SYM_TAU * ( (f32)i / (f32)segs );
        pts[ i ] = sv2( cx + cosf( a ) * r, cy + sinf( a ) * r );
    }
    gui_draw_polyline( pts, segs, thickness < 1.0f ? 1.0f : thickness, GUI_STROKE_CENTER, true, col );
}

/* Stroked arc from a0 to a1 (radians) -- a spinner sweep, a knob track, a radial-menu rim. */
static void
draw_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col )
{
    gui_vec2_t pts[ 66 ];
    u32 n = sym_arc( cx, cy, r, a0, a1, pts );
    gui_draw_polyline( pts, n, thickness < 1.0f ? 1.0f : thickness, GUI_STROKE_CENTER, false, col );
}

/* Filled pie / wedge from a0 to a1 (radians): a triangle fan from the centre over the arc.  A full
   sweep is a disc; a partial one is a pie slice (knobs, radial menus, donut segments). */
static void
draw_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col )
{
    gui_vec2_t pts[ 67 ];
    pts[ 0 ] = sv2( cx, cy );
    u32 n = sym_arc( cx, cy, r, a0, a1, pts + 1 );
    fill_convex( pts, n + 1, col );
}

/*----------------------------------------------------------------------------------------------
    Curves
----------------------------------------------------------------------------------------------*/

#define SYM_BEZIER_SEGS 24   /* flattening resolution for a bezier into a polyline */

/* Quadratic bezier from p0 through control c to p1, flattened to a stroked polyline (easing
   previews, simple wires). */
static void
draw_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col )
{
    gui_vec2_t pts[ SYM_BEZIER_SEGS + 1 ];
    for ( u32 i = 0; i <= SYM_BEZIER_SEGS; ++i )
    {
        f32 t = (f32)i / (f32)SYM_BEZIER_SEGS, u = 1.0f - t;
        pts[ i ] = sv2( u * u * x0 + 2.0f * u * t * cx + t * t * x1,
                        u * u * y0 + 2.0f * u * t * cy + t * t * y1 );
    }
    gui_draw_polyline( pts, SYM_BEZIER_SEGS + 1, thickness < 1.0f ? 1.0f : thickness,
                         GUI_STROKE_CENTER, false, col );
}

/* Cubic bezier from p0 with controls c0,c1 to p1, flattened to a stroked polyline (node-graph
   wires, S-curves). */
static void
draw_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y,
                   f32 x1, f32 y1, f32 thickness, u32 col )
{
    gui_vec2_t pts[ SYM_BEZIER_SEGS + 1 ];
    for ( u32 i = 0; i <= SYM_BEZIER_SEGS; ++i )
    {
        f32 t = (f32)i / (f32)SYM_BEZIER_SEGS, u = 1.0f - t;
        f32 b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
        pts[ i ] = sv2( b0 * x0 + b1 * c0x + b2 * c1x + b3 * x1,
                        b0 * y0 + b1 * c0y + b2 * c1y + b3 * y1 );
    }
    gui_draw_polyline( pts, SYM_BEZIER_SEGS + 1, thickness < 1.0f ? 1.0f : thickness,
                         GUI_STROKE_CENTER, false, col );
}

/*----------------------------------------------------------------------------------------------
    Patterned lines + fills
----------------------------------------------------------------------------------------------*/

/* Dashed / dotted line from (x0,y0) to (x1,y1): on-segments of length `dash` separated by `gap`
   (guides, selection marquees).  A small dash with gap == dash reads as dotted.  Backed by a single
   tiled textured quad (gui_draw_dashed_line) -- not one stroke per dash -- so length is free. */
static void
draw_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 col )
{
    gui_draw_dashed_line( x0, y0, x1, y1, dash, gap, thickness, col );
}

/* Checkerboard fill of `box` with `cell`-sized squares alternating col_a / col_b -- the classic
   transparency backdrop behind a color swatch.  Cell count is capped so a large area cannot flood
   the command list; partial edge cells are clamped to the box. */
static void
draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b )
{
    if ( cell < 1.0f ) cell = 1.0f;
    u32 cols = (u32)ceilf( box.w / cell ), rows = (u32)ceilf( box.h / cell );
    if ( cols > 64 ) cols = 64;
    if ( rows > 64 ) rows = 64;

    f32 save = draw_rounding();
    draw_set_rounding( 0.0f );
    for ( u32 yy = 0; yy < rows; ++yy )
        for ( u32 xx = 0; xx < cols; ++xx )
        {
            f32 px = box.x + xx * cell, py = box.y + yy * cell;
            f32 cw = px + cell > box.x + box.w ? box.x + box.w - px : cell;
            f32 ch = py + cell > box.y + box.h ? box.y + box.h - py : cell;
            draw_push_rect_filled( px, py, cw, ch, 0, 0, 1, 1, 0,
                                   ( ( xx + yy ) & 1u ) ? col_b : col_a );
        }
    draw_set_rounding( save );
}

/* Diagonal hatch fill of `box`: 45-degree lines `spacing` px apart (a disabled / read-only backdrop,
   a "no value" pattern).  Clipped to the box so the diagonals do not bleed past its edges. */
static void
draw_hatch( gui_rect_t box, f32 spacing, f32 thickness, u32 col )
{
    if ( spacing < 1.0f ) spacing = 1.0f;
    draw_push_clip_rect( box.x, box.y, box.w, box.h );
    f32 end = box.x + box.w;
    u32 guard = 0;
    for ( f32 x = box.x - box.h; x < end && guard < 512; x += spacing, ++guard )
        gui_draw_line( x, box.y, x + box.h, box.y + box.h, thickness, col );
    draw_pop_clip_rect();
}

/* Gradient fill of `box`, col_a -> col_b, vertical (default) or horizontal.  One quad whose
   opposite edges carry the two colors; the GPU's per-vertex color interpolation produces the
   smooth blend (draw_push_rect_gradient).  Square by nature -- the per-vertex blend has no
   rounded variant, matching the always-square fill this replaced. */
static void
draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal )
{
    draw_push_rect_gradient( box.x, box.y, box.w, box.h, col_a, col_b, horizontal );
}

/* Soft drop shadow / glow behind `box`: concentric expanded rects, alpha falling outward by
   `spread` px (popups, floating panels).  Approximated with a few layered rings on the single-color
   pipeline -- not a true gaussian, but reads as a soft edge; honors the ambient rounding so it hugs
   a rounded panel.  Draw it before the panel body. */
static void
draw_shadow( gui_rect_t box, f32 spread, u32 col )
{
    enum { LAYERS = 6 };
    u32 base_a = ( col >> 24 ) & 0xFFu;
    for ( u32 k = 0; k < LAYERS; ++k )
    {
        f32 g = spread * ( (f32)( LAYERS - k ) / (f32)LAYERS );          /* k=0 widest */
        u32 a = (u32)( (f32)base_a * ( (f32)( k + 1 ) / (f32)LAYERS ) * 0.5f );
        u32 cc = ( col & 0x00FFFFFFu ) | ( a << 24 );
        draw_push_rect_filled( box.x - g, box.y - g, box.w + 2.0f * g, box.h + 2.0f * g,
                               0, 0, 1, 1, 0, cc );
    }
}

/*----------------------------------------------------------------------------------------------
    Text effects + decorations
----------------------------------------------------------------------------------------------*/

/* Text with a 1px outline: the run drawn in col_outline at the 8 surrounding offsets, then in
   col_text on top -- legible over a busy / variable background. */
static void
draw_text_outline( f32 x, f32 y, const char* s, u32 col_text, u32 col_outline )
{
    static const f32 ox[ 8 ] = { -1, 1,  0, 0, -1, -1,  1, 1 };
    static const f32 oy[ 8 ] = {  0, 0, -1, 1, -1,  1, -1, 1 };
    for ( u32 i = 0; i < 8; ++i )
        draw_push_text( x + ox[ i ], y + oy[ i ], col_outline, s );
    draw_push_text( x, y, col_text, s );
}

/* Text with a single offset drop shadow (cheaper than a full outline; a soft lift off the panel). */
static void
draw_text_shadow( f32 x, f32 y, const char* s, u32 col_text, u32 col_shadow, f32 dx, f32 dy )
{
    draw_push_text( x + dx, y + dy, col_shadow, s );
    draw_push_text( x, y, col_text, s );
}

/* Resize grip dots: a triangular 1-2-3 cluster of small square dots in the lower-right of `box`,
   the familiar sizer texture (a window corner grip, a panel resize handle). */
static void
draw_grip_dots( gui_rect_t box, u32 col )
{
    f32 d = floorf( box.h * 0.16f );  if ( d < 2.0f ) d = 2.0f;   /* dot side  */
    f32 g = d * 2.0f;                                             /* dot pitch */
    f32 x1 = box.x + box.w - d, y1 = box.y + box.h - d;

    f32 save = draw_rounding();
    draw_set_rounding( 0.0f );
    for ( u32 row = 0; row < 3; ++row )
        for ( u32 c = 0; c <= row; ++c )                          /* row r has r+1 dots */
            draw_push_rect_filled( x1 - c * g, y1 - row * g, d, d, 0, 0, 1, 1, 0, col );
    draw_set_rounding( save );
}

/* Loading spinner: a 270-degree arc whose start angle advances with `t` seconds (caller supplies
   the time so the primitive stays stateless), fitted to `box`. */
static void
draw_spinner( gui_rect_t box, f32 t, f32 thickness, u32 col )
{
    f32 cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
    f32 r  = ( box.w < box.h ? box.w : box.h ) * 0.5f - thickness;
    if ( r < 1.0f ) r = 1.0f;
    f32 a0 = t * 6.0f;                  /* ~one revolution per second */
    draw_arc( cx, cy, r, a0, a0 + SYM_PI * 1.5f, thickness, col );
}

/* Progress arc: a ring filled clockwise from 12 o'clock by `frac` of a turn (a circular progress /
   gauge readout). */
static void
draw_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col )
{
    if ( frac < 0.0f ) frac = 0.0f;
    if ( frac > 1.0f ) frac = 1.0f;
    f32 a0 = -SYM_PI * 0.5f;            /* start at the top */
    draw_arc( cx, cy, r, a0, a0 + frac * SYM_TAU, thickness, col );
}

/*==============================================================================================
    Public surface -- the gui_draw_* family (Dear ImGui AddXxx / Render* analogue), drawn through the
    normal vertex pipeline (lines / triangles / circles), NOT the icon atlas.  Editor / custom
    widgets paint the same marks the built-in widgets use.  The set_*_style setters choose the
    global indicator shape; scope a change with push_style_var on the matching GUI_VAR_*_STYLE.
==============================================================================================*/

/* glyph marks */
void gui_draw_check_mark( gui_rect_t box, u32 col )                       { draw_check_mark( box, col ); }
void gui_draw_arrow     ( gui_rect_t box, gui_dir_t dir, u32 col )      { draw_arrow( box, dir, col ); }
void gui_draw_bullet    ( f32 cx, f32 cy, f32 r, u32 col )                  { draw_bullet( cx, cy, r, col ); }
void gui_draw_close     ( gui_rect_t box, u32 col )                       { draw_close_x( box, col ); }
void gui_draw_arrow_pointing_at( f32 tx, f32 ty, f32 half, gui_dir_t dir, u32 col )
                                                                               { draw_arrow_pointing_at( tx, ty, half, dir, col ); }
void gui_draw_chevron   ( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 col ) { draw_chevron( box, dir, thickness, col ); }
void gui_draw_plus_minus( gui_rect_t box, bool plus, f32 thickness, u32 col )       { draw_plus_minus( box, plus, thickness, col ); }

/* shapes */
void gui_draw_frame( gui_rect_t box, u32 col_bg, u32 col_border, f32 border ) { draw_frame( box, col_bg, col_border, border ); }

void
gui_draw_round_rect( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                         bool filled, f32 thickness, u32 col )
{
    /* Uniform-radius fast path: route a filled, equal-cornered rect through the backend's single
       rounded-rect command (crisp AA, one command) instead of fanning a perimeter into triangles. */
    if ( filled && r_tl == r_tr && r_tr == r_br && r_br == r_bl )
    {
        f32 save = draw_rounding();
        draw_set_rounding( r_tl );
        draw_push_rect_filled( box.x, box.y, box.w, box.h, 0, 0, 1, 1, 0, col );
        draw_set_rounding( save );
        return;
    }
    draw_round_rect_ex( box, r_tl, r_tr, r_br, r_bl, filled, thickness, col );
}

void gui_draw_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col )
                                                                               { draw_ngon( cx, cy, r, sides, rot, filled, thickness, col ); }
void gui_draw_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col ) { draw_circle( cx, cy, r, filled, thickness, col ); }
void gui_draw_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col )  { draw_arc( cx, cy, r, a0, a1, thickness, col ); }
void gui_draw_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col )                 { draw_pie( cx, cy, r, a0, a1, col ); }

/* curves */
void gui_draw_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col )
                                                                               { draw_bezier_quad( x0, y0, cx, cy, x1, y1, thickness, col ); }
void gui_draw_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col )
                                                                               { draw_bezier_cubic( x0, y0, c0x, c0y, c1x, c1y, x1, y1, thickness, col ); }

/* patterned lines + fills.  (draw_dashed_line has no wrapper here -- the public draw_dashed_line is
   the backend primitive in gui_01_emit_path.c; the vtable binds straight to it.  The file-local
   draw_dashed_line static below forwards to that same primitive for the separator rule.) */
void gui_draw_checker ( gui_rect_t box, f32 cell, u32 col_a, u32 col_b )  { draw_checker( box, cell, col_a, col_b ); }
void gui_draw_hatch   ( gui_rect_t box, f32 spacing, f32 thickness, u32 col ) { draw_hatch( box, spacing, thickness, col ); }
void gui_draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal ) { draw_gradient( box, col_a, col_b, horizontal ); }
void gui_draw_shadow  ( gui_rect_t box, f32 spread, u32 col )             { draw_shadow( box, spread, col ); }

/* text effects + decorations */
void gui_draw_text_outline( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline )
                                                                               { draw_text_outline( x, y, str, col_text, col_outline ); }
void gui_draw_text_shadow( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy )
                                                                               { draw_text_shadow( x, y, str, col_text, col_shadow, dx, dy ); }
void gui_draw_grip( gui_rect_t box, u32 col )                            { draw_grip_dots( box, col ); }
void gui_draw_spinner( gui_rect_t box, f32 t, f32 thickness, u32 col )    { draw_spinner( box, t, thickness, col ); }
void gui_draw_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col ) { draw_progress_arc( cx, cy, r, frac, thickness, col ); }

/* global indicator-shape setters (gui_check_style_t / gui_bullet_style_t / gui_arrow_style_t) */
void gui_set_check_style ( u8 style ) { s_style.check_style  = style; }
void gui_set_bullet_style( u8 style ) { s_style.bullet_style = style; }
void gui_set_arrow_style ( u8 style ) { s_style.arrow_style  = style; }

// clang-format on
/*============================================================================================*/
