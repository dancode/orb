/*==============================================================================================

    runtime_service/gui/draw/gui_symbol.c -- Symbol + shape render primitives.

    The frontend "render route" palette: the small glyph marks the chrome draws (arrows, check,
    bullet, close, pointer beak) plus the broader shape family editor / custom widgets reach for
    (frames, per-corner rounded rects, regular polygons, circles / rings / arcs / pie wedges,
    bezier curves, dashed lines, checker / hatch fills, gradients, soft shadows, outlined / shadowed
    text, resize grips, spinners, progress arcs).  This is the Dear ImGui Render* / AddXxx family.

    These compose the *backend* primitives (draw_push_triangle / _circle_filled / _rect_filled /
    _rect_outline / _text, gui_draw_line / gui_draw_polyline) into named marks -- they draw
    through the normal quad path, NOT the runtime icon atlas (gui_icon.c).  Two routes do
    the heavy lifting: a triangle fan (sym_fill_convex) fills any convex outline, and a closed / open
    polyline strokes it; arcs are sampled from cos / sin once per call.

    Most commands carry one abgr, but GUI_CMD_RECT_GRADIENT carries two and hands the ramp to the
    FRAGMENT through the style record (GUI_OP_GRAD), so draw_gradient is an exact one-quad blend
    (not banded).  draw_shadow and draw_round_rect hand their shape to the FRAGMENT (GUI_CMD_FX_BOX / a
    rounded rect command, both SDF surfaces -- see the effect band in gui.h): exact edges at any
    radius and softness, ONE quad, no batch split.  Only a STROKED per-corner outline still walks a
    tessellated perimeter here.  Everything here is pixel-exact.

    Compiled in the DRAW unit (gui_draw.c) after gui_paint.c.  Everything here is
    PARAMETER-PURE: the emitters that resolve their own look (draw_arrow,
    draw_check_indicator, draw_rule, draw_close_x, draw_frame -- style-var picks, WIN_BORDER,
    ROUND_WIDGET) live in stock/gui_symbol_style.c.  The public gui_draw_* surface over
    the pure set is at the foot.

==============================================================================================*/
// clang-format off

#define SYM_PI   3.14159265358979f
#define SYM_TAU  ( 2.0f * SYM_PI )

/*==============================================================================================
    Shared geometry helpers
==============================================================================================*/

/* Make a vec2 (the render server owns its own v2; this is the draw unit's local one). */
static gui_vec2_t sv2( f32 x, f32 y ) { return ( gui_vec2_t ){ x, y }; }

/* Stroke width floored at one pixel.  A sub-pixel thickness strokes to nothing, so every mark
   that takes a caller thickness passes it through here -- one rule, not ten open-coded clamps. */
static f32 sym_thick( f32 thickness ) { return thickness < 1.0f ? 1.0f : thickness; }

/* The box's inscribed extent -- the shorter side, which is what a square mark (arrow, plus,
   spinner) sizes itself against so it stays square in a non-square cell. */
static f32 sym_min_side( gui_rect_t box ) { return box.w < box.h ? box.w : box.h; }

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
sym_fill_convex( const gui_vec2_t* pts, u32 n, u32 col )
{
    for ( u32 i = 1; i + 1 < n; ++i )
        draw_push_triangle( pts[ 0 ].x, pts[ 0 ].y, pts[ i ].x, pts[ i ].y,
                            pts[ i + 1 ].x, pts[ i + 1 ].y, col );
}

/*==============================================================================================
    Glyph marks  (the chrome's symbol set; the Dear ImGui Render* glyphs)
==============================================================================================*/

/* Chevron glyph: a stroked '>' pointing `dir`, centered in `box` (the open sibling of the filled
   arrow).  Three points -- the two back corners and the apex -- stroked as an open polyline, sized
   to the same half-extent as draw_arrow so the two styles are interchangeable at any font size. */
static void
draw_chevron( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 color )
{
    gui_vec2_t c  = gui_rect_center( box );
    f32        cx = c.x, cy = c.y;
    f32        s  = floorf( box.h * 0.22f );
    thickness = sym_thick( thickness );

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

/* draw_arrow + draw_collapse_arrow (the GUI_VAR_ARROW_SHAPE pick) live in
   stock/gui_symbol_style.c; the chevron variant routes back through
   gui_draw_chevron below. */

/* Check-mark glyph: a two-stroke 'v' fitted and centered in `box` (Dear ImGui RenderCheckMark).
   Two antialiased line segments -- a short down-stroke into the valley, then a long up-stroke --
   the same line primitive the close 'X' uses, so ticks and crosses stroke identically.  The
   geometry is expressed as fractions of the fitted square so it scales crisply at any box size. */
static void
draw_check_mark( gui_rect_t box, u32 color )
{
    f32 sz = sym_min_side( box );
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

/* Dropdown-arrow glyph: a soft downward 'v' -- two open strokes, not a filled triangle (that reads
   as the collapse/expand glyph) -- fitted and centered in `box` (the toolbar split-button's
   affordance mark).  Scales off min(w,h) like draw_check_mark rather than off box.h alone like
   draw_arrow, so it drops cleanly into a narrow side column without overrunning the column width.
   Fixed DOWN orientation -- unlike draw_arrow it is not a general direction glyph and does not
   follow GUI_VAR_ARROW_SHAPE; the toolbar affordance stays this one mark regardless of theme. */
void
draw_dropdown_arrow( gui_rect_t box, u32 color )
{
    gui_vec2_t c  = gui_rect_center( box );
    f32        cx = c.x, cy = c.y;
    f32        sz = sym_min_side( box );
    f32        s  = floorf( sz * 0.22f );   /* half-extent */
    f32 t  = floorf( sz * 0.15f );  if ( t < 1.5f ) t = 1.5f;   /* stroke thickness */

    gui_draw_line( cx - s, cy - s * 0.4f, cx, cy + s * 0.6f, t, color );
    gui_draw_line( cx, cy + s * 0.6f, cx + s, cy - s * 0.4f, t, color );
}

/* Bullet glyph: a small filled disc centered at (cx,cy) (Dear ImGui RenderBullet).  The round
   sibling of the square bullet -- the bullet widget picks between them on GUI_VAR_BULLET_SHAPE. */
void
draw_bullet( f32 cx, f32 cy, f32 r, u32 color )
{
    draw_push_circle_filled( cx, cy, r, color );
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
        case GUI_DIR_LEFT:  draw_push_triangle( tx, ty, tx + half, ty - half, tx + half, ty + half, color ); break;
        case GUI_DIR_RIGHT: draw_push_triangle( tx, ty, tx - half, ty - half, tx - half, ty + half, color ); break;
        case GUI_DIR_UP:    draw_push_triangle( tx, ty, tx - half, ty + half, tx + half, ty + half, color ); break;
        case GUI_DIR_DOWN:  draw_push_triangle( tx, ty, tx - half, ty - half, tx + half, ty - half, color ); break;
    }
}

/* Plus / minus glyph: a horizontal bar, plus a vertical bar for the '+' form, centered in `box`
   (tree expand / collapse, zoom in / out).  Strokes so it tracks the arrow / chevron weight. */
static void
draw_plus_minus( gui_rect_t box, bool plus, f32 thickness, u32 color )
{
    gui_vec2_t c  = gui_rect_center( box );
    f32        cx = c.x, cy = c.y;
    f32        s  = floorf( sym_min_side( box ) * 0.3f );
    thickness = sym_thick( thickness );

    gui_draw_line( cx - s, cy, cx + s, cy, thickness, color );
    if ( plus )
        gui_draw_line( cx, cy - s, cx, cy + s, thickness, color );
}

/*==============================================================================================
    Shapes  (the convex-fill / polyline-stroke palette)
==============================================================================================*/

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

/* Per-corner rounded rect, filled or stroked.  The general path for tab / notch / asymmetric
   shapes; for a uniform radius prefer the public draw_round_rect, which delegates to the backend's
   single-command rounded rect.

   The two halves no longer share a path, and the split is the point.  FILLED is a distance-field
   surface -- one command, four vertices, an exact antialiased boundary at any radius -- because
   all four radii ride the record and the fragment picks by quadrant, so they cost no
   extra geometry.  It used to fan the sampled perimeter into up to 62 separate TRIANGLE commands
   with a polygonal, unantialiased edge.  STROKED still walks the perimeter: an outline of four
   different radii is not a shape GUI_OP_BAND can describe (its band is derived from one), and the
   closed antialiased polyline draws it correctly already. */
void
draw_round_rect_ex( gui_rect_t b, f32 rtl, f32 rtr, f32 rbr, f32 rbl,
                    bool filled, f32 thickness, u32 col )
{
    if ( filled )
    {
        draw_push_round_rect_ex( b.x, b.y, b.w, b.h, rtl, rtr, rbr, rbl, 0.0f, col, col,
                                 0.0f, (u32)GUI_GRAD_LINEAR, 0.0f );
        return;
    }
    gui_vec2_t pts[ 4 * 17 + 4 ];
    u32 n = round_rect_perimeter_ex( b, rtl, rtr, rbr, rbl, pts );
    gui_draw_polyline( pts, n, sym_thick( thickness ), GUI_STROKE_CENTER, true, col );
}

/* Regular n-gon centred at (cx,cy), circumradius r, first vertex at angle `rot` -- generalizes
   the triangle / diamond / hexagon marks.
   A signed-distance surface now, filled and stroked both -- the sampled fan and its 64-point
   ribbon are gone.  The boundary is exact at any size, both forms antialias, and the corners
   round by the ambient rounding (a design tool's rounded hexagon badge), which the fan never
   could.  `rot` keeps the sampled convention: 0 puts a vertex at +x, the angle algebra every
   caller already speaks -- the field's own reference (a vertex up) differs by a quarter turn,
   folded in here. */
static void
draw_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col )
{
    draw_push_ngon( cx, cy, r, sides, rot + SYM_PI * 0.5f, draw_rounding(),
                    filled ? 0.0f : sym_thick( thickness ), col );
}

/* Circle at arbitrary radius: filled or stroked (a ring of `thickness`).  The ring is the outlined
   form -- pass filled = false with a thickness.

   NEITHER form samples the circle any more.  Both are signed-distance surfaces the fragment
   resolves, so the boundary is exact at any radius and the geometry is fixed-cost: what used to be
   "how many segments can we afford" is not a question this function has to answer.  It matters most
   at the SMALL end, which is where these actually get used -- sym_arc_segs gives a 10 px mark ten
   segments, so a "circle" that size was a visible decagon.

       ring r = 10   40 verts / 180 idx  ->  32 / 48
       ring r = 100  256 verts / 1152 idx -> 32 / 48

   A ring is a rounded rect whose radius reached its half-extent, which is why it needs no shape of
   its own: GUI_OP_BAND already paints a band `border` px wide lying INSIDE the boundary.  Two
   details make it match what the polyline drew:
     - GUI_STROKE_CENTER centres the band ON radius r, so the band spans [r - t/2, r + t/2].  The
       SDF band hangs inside its boundary, so the boundary is r + t/2.
     - tess_fx_box does not grid-snap a circle (it derives that), so a ring and a filled disc at the
       same centre stay concentric to the sub-pixel.  Concentric marks are the common case here. */
void
draw_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col )
{
    if ( filled )
    {
        draw_push_circle_filled( cx, cy, r, col );
        return;
    }

    thickness = sym_thick( thickness );

    /* EVERY thickness takes the ring now.  This used to be gated at 15.875 px -- what the packed
       word's border field could describe -- with anything fatter falling back to a stroked polyline
       of up to 64 segments.  The record's border is a plain float, so the gate has nothing left to
       catch and the fallback is gone with it. */
    f32 outer = r + thickness * 0.5f;
    /* draw_push_rect_outline takes its radius from the AMBIENT rounding, clamped to half the
       extent -- so asking for the full half-extent is how a square becomes a circle here. */
    f32 save = draw_rounding();
    draw_set_rounding( outer );
    draw_push_rect_outline( cx - outer, cy - outer, outer * 2.0f, outer * 2.0f, thickness, col );
    draw_set_rounding( save );
}

/* Stroked arc from a0 to a1 (radians) -- a spinner sweep, a knob track, a radial-menu rim.

   A distance field now, so the curve is exact at any radius and the cost is four vertices instead
   of the ~130 the sampled ribbon spent.  It matters most where these actually get used: sym_arc_segs
   gave a 12 px spinner about ten segments, so the "circle" it swept was a visible decagon. */
static void
draw_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col )
{
    thickness = sym_thick( thickness );

    /* Every thickness takes the SDF arc, for the same reason draw_circle's ring does: the tube is a
       plain float in the record, so the 15.875 px ceiling that used to send fat arcs down the
       stroked-polyline path is gone, and so is that path. */
    draw_push_arc( cx, cy, r, thickness, a0, a1, col );
}

/* Filled pie / wedge from a0 to a1 (radians): knobs, radial menus, donut segments.  A full sweep
   is a disc.

   This was the worst offender in the library.  A fan over the sampled arc emitted n-2 SEPARATE
   triangle commands -- up to 65 of a 1024 budget for ONE shape, with a polygonal rim and no
   antialiasing at all.  The wedge is now a single quad whose radial edges stay sharp and whose rim
   is exact, which is the combination a fan could never give: rounding the caps would have been
   wrong here, and a fan could not round anything. */
static void
draw_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col )
{
    draw_push_pie( cx, cy, r, a0, a1, col );
}

/*==============================================================================================
    Curves
==============================================================================================*/

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
    gui_draw_polyline( pts, SYM_BEZIER_SEGS + 1, sym_thick( thickness ),
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
    gui_draw_polyline( pts, SYM_BEZIER_SEGS + 1, sym_thick( thickness ),
                         GUI_STROKE_CENTER, false, col );
}

/*==============================================================================================
    Patterned lines + fills
==============================================================================================*/

/* Checkerboard fill of `box` with `cell`-sized squares alternating col_a / col_b -- the classic
   transparency backdrop behind a color swatch.  ONE quad: the fragment tiles the pattern in
   framebuffer space (GUI_OP_CHECKER), so any area at any cell costs four vertices.  This used to
   be a rect-pool expansion capped at 64x64 cells -- 64 commands and up to 4096 quads per call,
   with the pattern coarsening past the clamp; both the cost and the clamp are gone. */
void
draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b )
{
    draw_push_checker( box.x, box.y, box.w, box.h, cell, col_a, col_b );
}

/* Line lattice over `box`: a `thickness` px line every `cell` px, drawn over NOTHING -- layer it
   on your own fill (two quads make the classic node-graph backdrop: a fill and this).  The
   lattice anchors to (origin_x, origin_y) in screen px, so a panning canvas passes its content
   origin and the grid rides the pan; a static backdrop passes the box origin.  For major/minor
   graph paper, draw twice at two pitches. */
void
draw_grid( gui_rect_t box, f32 cell, f32 thickness, f32 origin_x, f32 origin_y, u32 col )
{
    draw_push_grid( box.x, box.y, box.w, box.h, origin_x, origin_y, 0.0f, false,
                    cell, thickness, col );
}

/* Diagonal hatch fill of `box`: 45-degree lines `spacing` px apart (a disabled / read-only backdrop,
   a "no value" pattern).  Clipped to the box so the diagonals do not bleed past its edges. */
static void
draw_hatch( gui_rect_t box, f32 spacing, f32 thickness, u32 col )
{
    if ( spacing < 1.0f ) spacing = 1.0f;
    draw_push_grid( box.x, box.y, box.w, box.h, box.x, box.y,
                    SYM_PI * 0.25f, true, spacing, thickness, col );
}

/* Stripes across `box`: parallel lines `spacing` px apart, `thickness` wide, running at `angle`
   (radians; 0 gives vertical lines, and the hatch above is simply this at 45 degrees).  Anchored
   to the box origin, so the pattern travels with the shape rather than sliding under it.

   One quad, resolved in the fragment (GUI_OP_GRID with its stripe bit).  The lines are a lattice
   cut on ONE axis, which is why an arbitrary angle costs nothing extra -- the fragment rotates
   its own pixel coordinate and everything downstream is the axis-aligned case. */
void
draw_stripes( gui_rect_t box, f32 spacing, f32 thickness, f32 angle, u32 col )
{
    if ( spacing < 1.0f ) spacing = 1.0f;
    draw_push_grid( box.x, box.y, box.w, box.h, box.x, box.y,
                    angle, true, spacing, thickness, col );
}

/* Gradient fill of `box`, col_a -> col_b, vertical (default) or horizontal.  One quad whose
   style record carries the ramp (GUI_OP_GRAD), resolved per pixel in the fragment
   (draw_push_rect_gradient).  Square and axis-aligned -- draw_round_rect_gradient
   below is the general form. */
void
draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal )
{
    draw_push_rect_gradient( box.x, box.y, box.w, box.h, col_a, col_b, horizontal );
}

/* Gradient fill of a ROUNDED `box`, col_a -> col_b.  `kind` shapes the ramp and `angle` orients it
   (radians, 0 points +x, positive turns clockwise): the axis for GUI_GRAD_LINEAR, the direction the
   sheen peaks toward for GUI_GRAD_CONIC, ignored by GUI_GRAD_RADIAL.  A linear ramp spans the box
   along its axis and holds its end colors past it.

   The rounded SDF surface and the ramp cost each other nothing: this is the same one quad and the
   same draw call a flat draw_round_rect produces, with the ramp resolved from the record in the
   fragment -- which is why a gradient is affordable on ordinary chrome rather than a special
   occasion, and why radial and conic are available at all. */
/* `mid` is the ramp's midpoint, 0..1: where along the ramp the 50/50 blend lands (a design
   tool's gradient midpoint handle).  0.5 -- and 0, the unset default -- is the linear ramp. */
void
draw_round_rect_gradient( gui_rect_t box, f32 rounding, u32 col_a, u32 col_b,
                          gui_grad_t kind, f32 angle, f32 mid )
{
    draw_push_round_rect_ex( box.x, box.y, box.w, box.h,
                             rounding, rounding, rounding, rounding, 0.0f,
                             col_a, col_b, angle, (u32)kind, mid );
}

/* The dashed rounded border -- and at a non-zero `speed` (px/sec) the marching ants, scrolling
   on the shader clock with no re-tessellation (the caller presents frames, the pulse contract).
   dash/gap are arc-length px, the draw_dashed_line vocabulary; the period is snapped so whole
   cycles fit the perimeter and a closed border meets itself.  Honors the border-align ambient. */
void
draw_round_rect_dashed( gui_rect_t box, f32 rounding, f32 thickness,
                        f32 dash, f32 gap, f32 speed, u32 col )
{
    draw_push_box_dashed( box.x, box.y, box.w, box.h, rounding, sym_thick( thickness ),
                          dash, gap, speed, 0.0f, col );
}

/* The border tracer: one arc of `frac` of the outline travelling around it at `rate` laps/sec --
   indeterminate progress that traces the shape it belongs to instead of sitting beside it as a
   bar.  The dashed border with a single cycle spanning the whole perimeter, so it costs the same
   one quad, and its command's bytes never change while it runs: the arc moves in the fragment on
   the shader clock and re-tessellates nothing.  The caller keeps frames presenting with
   gui()->request_redraw() while it shows, the draw_pulse contract. */
void
draw_border_tracer( gui_rect_t box, f32 rounding, f32 thickness, f32 frac, f32 rate, u32 col )
{
    if ( frac <= 0.0f ) frac = 0.08f;
    draw_push_box_trace( box.x, box.y, box.w, box.h, rounding, sym_thick( thickness ),
                         frac, rate, 0.0f, col );
}

/* The determinate twin: the same arc positioned by a VALUE rather than by the clock.  `t` is
   0..1 around the border from the top-left corner, so a load that reports its progress traces the
   outline of the thing being loaded.  Unlike the tracer this re-tessellates its window when `t`
   moves -- the value is in the command's bytes, exactly as a progress bar's fill width is. */
void
draw_border_progress( gui_rect_t box, f32 rounding, f32 thickness, f32 frac, f32 t, u32 col )
{
    if ( frac <= 0.0f ) frac = 0.08f;
    draw_push_box_trace( box.x, box.y, box.w, box.h, rounding, sym_thick( thickness ),
                         frac, 0.0f, t, col );
}

/* Inner shadow inside `box`, strongest against the edge and gone `depth` px in, with nothing
   painted outside.  The mirror of draw_shadow: that one lays a shape on the ground under its
   subject, this one lays it against the inside of the subject's own edge (pressed wells, recessed
   fields, the inner lip of a scroll area).  Takes the ambient rounding like draw_shadow does.

   The interior is HOLLOW -- the band is only `depth` deep, so an inset on a full-size panel is
   covered by a frame of quads around the rim rather than one spanning it. */
void
draw_inset_shadow( gui_rect_t box, f32 depth, u32 col )
{
    draw_push_inset( box.x, box.y, box.w, box.h, draw_rounding(), depth, col );
}

/* Soft drop shadow / glow behind `box`, alpha falling off over `spread` px (popups, floating
   panels).  ONE SDF surface: the fragment shader resolves the falloff exactly, so this is no
   longer the six stacked rects that used to stand in for a gaussian -- and it costs one quad
   in whatever batch is already open rather than six commands of its own.  Honors the ambient
   rounding so it hugs a rounded panel.  Draw it before the panel body.

   `spread` reads as "how far the shadow reaches", so it is HALF the falloff band: the fill is
   solid until spread px inside the box and gone spread px outside it.

   The core is FILLED, which is what a glow wants -- a halo behind a transparent card, a hover
   bloom under a tile -- and is the reason this is not the skirt the window elevation shadow uses
   (draw_push_skirt): a shape meant to be seen through its subject must paint under it. */
static void
draw_shadow( gui_rect_t box, f32 spread, u32 col )
{
    draw_push_shadow( box.x, box.y, box.w, box.h, draw_rounding(), spread * 2.0f, col );
}

/* A glow behind `box`: the same surface draw_shadow lays down, with its falloff resolved
   EXPONENTIALLY instead of linearly.  That one substitution is the whole difference -- light falls
   off by a constant fraction per pixel, and the eye reads an exponential halo as emission where it
   reads a linear one as blur.  Same single quad, same batch, same cost.

   `spread` is how far the light reaches, the draw_shadow vocabulary.  The core is filled, so a
   glow behind a translucent subject shows through it; put it before the body like a shadow.
   Compose it with draw_pulse's clock for a breathing glow that re-tessellates nothing. */
static void
draw_glow( gui_rect_t box, f32 spread, u32 col )
{
    draw_push_glow( box.x, box.y, box.w, box.h, draw_rounding(), spread * 2.0f, col );
}

/* The drop shadow proper: the same falloff with the CASTER'S silhouette cut out of it, laid
   (off_x, off_y) px away from the box that casts it.  Nothing paints inside `box` itself, so a
   translucent panel shows what is behind it rather than its own shadow's core -- and because the
   cut is taken against the caster while the falloff is measured from the shadow, the cast can have
   a DIRECTION.  (0, 0) is the even cast on all four sides.

   `spread` reads as "how far the shadow reaches" and is HALF the falloff band, the draw_shadow
   rule.  Honors the ambient rounding so it hugs a rounded panel; draw it before the panel body. */
static void
draw_drop_shadow( gui_rect_t box, f32 spread, f32 off_x, f32 off_y, u32 col )
{
    draw_push_skirt( box.x, box.y, box.w, box.h, draw_rounding(), spread * 2.0f,
                     off_x, off_y, col );
}

/* A rounded fill whose alpha breathes, evaluated in the FRAGMENT off the shared frame clock.
   The caller-visible difference from animating the color yourself is what it does NOT do: the
   command's bytes are identical every frame, so the window's retained geometry is never
   invalidated and the pulse re-tessellates nothing.  It is the cheap way to keep something
   asking for attention on a screen full of static widgets.

   `rate` is in Hz (quantized to 1/4 Hz) and `depth` the 0..1 fraction of alpha taken at the
   trough.  Honors the ambient rounding.  The frame still has to be presented -- the caller runs
   gui()->request_redraw() while the pulse is live (see GUI_FX_TIME_WRAP in gui.h). */
/* `phase` offsets the wave in cycles, so a row of same-rate indicators can stagger instead of
   beating in lockstep; 0 keeps them synchronized. */
static void
draw_pulse( gui_rect_t box, f32 rate, f32 depth, f32 phase, u32 col )
{
    draw_push_pulse( box.x, box.y, box.w, box.h, draw_rounding(), rate, depth, phase, col );
}

/*==============================================================================================
    Text effects + decorations
==============================================================================================*/

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

/* A lattice of one cell: nx by ny copies `pitch` apart, centred in `at`.  ONE quad and one style
   record whatever the count, so the tick strips and dot fields that used to be priced per copy are
   priced per STRIP.  `size` is the cell's side; the pitch is floored just above it so copies never
   touch. */
static void
draw_dot_grid( gui_rect_t at, u32 nx, u32 ny, f32 pitch_x, f32 pitch_y, f32 size, u32 col )
{
    gui_vec2_t c = gui_rect_center( at );
    draw_push_repeat( c.x, c.y, nx, ny, pitch_x, pitch_y, size, size, draw_rounding(), col );
}

/* N tick marks across `bar` -- ruler gradations, slider detents, timeline marks.  The pitch is
   solved from the count and the span so the first and last ticks land on the ends, and the whole
   strip is one quad: a 40-tick ruler costs exactly what a 4-tick one does. */
static void
draw_ticks( gui_rect_t bar, u32 n, f32 thickness, f32 len, bool vertical, u32 col )
{
    if ( n == 0u )
        return;

    f32 t = sym_thick( thickness );
    if ( len <= 0.0f ) len = vertical ? bar.w : bar.h;

    /* One tick has no span to divide, so it sits in the middle rather than at an end. */
    f32 span = ( vertical ? bar.h : bar.w ) - t;

    /* Ticks that would touch get their pitch floored apart, which would push the strip past the
       bar it was fitted to.  Cap the count at what actually fits: a ruler's bounds are the
       contract, and a caller asking for more gradations than there are pixels wants the densest
       ruler that still fits, not one that overflows its lane. */
    if ( n > 2u && span > 0.0f )
    {
        u32 nmax = (u32)( span / t );
        if ( nmax < 2u ) nmax = 2u;
        if ( n > nmax )  n = nmax;
    }

    f32 pitch = ( n > 1u ) ? span / (f32)( n - 1u ) : 0.0f;

    gui_vec2_t c = gui_rect_center( bar );
    if ( vertical )
        draw_push_repeat( c.x, c.y, 1u, n, t, pitch, len, t, 0.0f, col );
    else
        draw_push_repeat( c.x, c.y, n, 1u, pitch, t, t, len, 0.0f, col );
}

/* A ring of `n` dots fitted to `box`, turning at `rate` revolutions/sec on the SHADER CLOCK.  One
   quad and one style record for the whole ring, and the command's bytes are identical every frame
   while it turns -- so unlike a hand-rotated ring of circles it re-tessellates nothing.  rate 0 is
   a static ring; the caller presents frames with gui()->request_redraw() while it spins, the
   draw_pulse contract.

   The ring turns as a rigid body: every dot shares one record and one colour, so this is the
   mechanical spinner rather than the one with a bright head chasing a faded tail.  Pair it with a
   STAIR curve of `n` steps and it advances exactly one dot per tick. */
static void
draw_dot_spinner( gui_rect_t box, u32 n, f32 dot, f32 rate, u32 col )
{
    gui_vec2_t c = gui_rect_center( box );
    f32        d = ( dot > 1.0f ) ? dot : 1.0f;
    f32        r = sym_min_side( box ) * 0.5f - d * 0.5f;
    if ( r < 1.0f ) r = 1.0f;

    /* Round cells: the radius reaches the cell's half-extent, which is what makes a dot a dot. */
    draw_push_repeat_polar( c.x, c.y, n, r, d, d, d * 0.5f, rate, 0.0f, col );
}

/* A dial face: `n` tick marks on a circle fitted to `box`, each `len` px long and pointing
   OUTWARD.  The same ring as above with a rectangular cell -- the polar fold turns each copy's
   frame with its position, so radial orientation is not something this has to state.  `rate` spins
   it (0 = a static gauge face). */
static void
draw_dial_ticks( gui_rect_t box, u32 n, f32 thickness, f32 len, f32 rate, u32 col )
{
    gui_vec2_t c = gui_rect_center( box );
    f32        t = sym_thick( thickness );
    f32        l = ( len > 1.0f ) ? len : 1.0f;
    f32        r = sym_min_side( box ) * 0.5f - l * 0.5f;
    if ( r < 1.0f ) r = 1.0f;

    /* The cell is long on the fold's +x axis, which the fold points away from the centre. */
    draw_push_repeat_polar( c.x, c.y, n, r, l, t, 0.0f, rate, 0.0f, col );
}

/* Resize grip dots: a triangular 1-2-3 cluster of small square dots in the lower-right of `box`,
   the familiar sizer texture (a window corner grip, a panel resize handle).  Each ROW of the
   cluster is one lattice, so the cluster is three quads rather than six -- the rows have different
   lengths, which is the one thing a single lattice cannot say. */
static void
draw_grip_dots( gui_rect_t box, u32 col )
{
    f32 d = floorf( box.h * 0.16f );  if ( d < 2.0f ) d = 2.0f;   /* dot side  */
    f32 g = d * 2.0f;                                             /* dot pitch */
    f32 x1 = box.x + box.w - d, y1 = box.y + box.h - d;

    for ( u32 row = 0; row < 3; ++row )                           /* row r has r+1 dots */
        draw_push_repeat( x1 + d * 0.5f - (f32)row * g * 0.5f, y1 + d * 0.5f - (f32)row * g,
                          row + 1u, 1u, g, g, d, d, 0.0f, col );
}

/* Loading spinner: a 270-degree arc turning at `rate` revolutions/sec, fitted to `box`.  The
   rotation runs on the SHADER CLOCK (GUI_OP_SPIN), so the command's bytes are identical every
   frame and the spin re-tessellates nothing -- the last animated primitive that used to re-emit
   per frame.  The caller keeps frames presenting with gui()->request_redraw() while it shows,
   the pulse contract.  rate <= 0 takes one turn per second. */
static void
draw_spinner( gui_rect_t box, f32 rate, f32 thickness, u32 col )
{
    gui_vec2_t c = gui_rect_center( box );
    f32        r = sym_min_side( box ) * 0.5f - thickness;
    if ( r < 1.0f ) r = 1.0f;
    if ( rate <= 0.0f ) rate = 1.0f;
    draw_push_arc_spin( c.x, c.y, r, sym_thick( thickness ),
                        0.0f, SYM_PI * 1.5f, rate, 0.0f, col );
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
    widgets paint the same marks the built-in widgets use.  The styled pieces of the family
    (arrow / close / frame wrappers, the set_*_style setters) live with their targets in the
    element and style units.
==============================================================================================*/

/* glyph marks */
void gui_draw_check_mark( gui_rect_t box, u32 col )                       { draw_check_mark( box, col ); }
void gui_draw_bullet    ( f32 cx, f32 cy, f32 r, u32 col )                  { draw_bullet( cx, cy, r, col ); }
void gui_draw_arrow_pointing_at( f32 tx, f32 ty, f32 half, gui_dir_t dir, u32 col )
                                                                               { draw_arrow_pointing_at( tx, ty, half, dir, col ); }
void gui_draw_chevron   ( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 col ) { draw_chevron( box, dir, thickness, col ); }
void gui_draw_plus_minus( gui_rect_t box, bool plus, f32 thickness, u32 col )       { draw_plus_minus( box, plus, thickness, col ); }

/* shapes */
void
gui_draw_round_rect( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                         bool filled, f32 thickness, u32 col )
{
    /* Uniform-radius fast path: route an equal-cornered rect -- filled or stroked -- through the
       backend's single rounded-rect command, which is an SDF surface (one quad, exact analytic
       AA).  Asymmetric corners go to draw_round_rect_ex, which is a surface too when FILLED; only
       its stroked form pays a tessellated arc and a polyline. */
    bool equal_corners = ( r_tl == r_tr && r_tr == r_br && r_br == r_bl );
    if ( equal_corners )
    {
        f32 save = draw_rounding();
        draw_set_rounding( r_tl );
        if ( filled ) draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0, 0, 1, 1, 0, col );
        else          draw_push_rect_outline( box.x, box.y, box.w, box.h,
                                              sym_thick( thickness ), col );
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

/* The self-sampled sector variants -- both bind straight to their backend primitives; the emit
   side owns the dash quantization and the gradient's reversed-range colour swap. */
void gui_draw_arc_dashed( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness,
                          f32 dash, f32 gap, u32 col )
{
    thickness = sym_thick( thickness );
    draw_push_arc_dashed( cx, cy, r, thickness, a0, a1, dash, gap, col );
}
void gui_draw_arc_gradient( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness,
                            u32 col_a, u32 col_b )
{
    thickness = sym_thick( thickness );
    draw_push_arc_gradient( cx, cy, r, thickness, a0, a1, col_a, col_b );
}

/* The rotated SDF box, and the per-corner soft shadow -- the two rect-family verbs the effect
   band supported all along and nothing exposed. */
void gui_draw_box_xf( gui_rect_t box, f32 rounding, f32 feather, f32 rot, u32 col )
{
    draw_push_box_xf( box.x, box.y, box.w, box.h, rounding, feather, rot, col );
}
void gui_draw_round_rect_shadow( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                                 f32 feather, u32 col )
{
    draw_push_round_rect_ex( box.x, box.y, box.w, box.h, r_tl, r_tr, r_br, r_bl, feather,
                             col, col, 0.0f, (u32)GUI_GRAD_LINEAR, 0.0f );
}

/* curves */
void gui_draw_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col )
                                                                               { draw_bezier_quad( x0, y0, cx, cy, x1, y1, thickness, col ); }
void gui_draw_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col )
                                                                               { draw_bezier_cubic( x0, y0, c0x, c0y, c1x, c1y, x1, y1, thickness, col ); }

/* patterned lines + fills.  (gui_draw_dashed_line is the backend primitive in gui_emit_path.c;
   the vtable binds straight to it.) */
void gui_draw_checker ( gui_rect_t box, f32 cell, u32 col_a, u32 col_b )  { draw_checker( box, cell, col_a, col_b ); }
void gui_draw_grid    ( gui_rect_t box, f32 cell, f32 thickness, f32 origin_x, f32 origin_y, u32 col )
                                                                               { draw_grid( box, cell, thickness, origin_x, origin_y, col ); }
void gui_draw_hatch   ( gui_rect_t box, f32 spacing, f32 thickness, u32 col ) { draw_hatch( box, spacing, thickness, col ); }
void gui_draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal ) { draw_gradient( box, col_a, col_b, horizontal ); }
void gui_draw_round_rect_gradient( gui_rect_t box, f32 rounding, u32 col_a, u32 col_b, gui_grad_t kind, f32 angle, f32 mid ) { draw_round_rect_gradient( box, rounding, col_a, col_b, kind, angle, mid ); }
void gui_draw_round_rect_dashed( gui_rect_t box, f32 rounding, f32 thickness, f32 dash, f32 gap, f32 speed, u32 col )
                                                                               { draw_round_rect_dashed( box, rounding, thickness, dash, gap, speed, col ); }
void gui_draw_border_tracer( gui_rect_t box, f32 rounding, f32 thickness, f32 frac, f32 rate, u32 col )
                                                                               { draw_border_tracer( box, rounding, thickness, frac, rate, col ); }
void gui_draw_border_progress( gui_rect_t box, f32 rounding, f32 thickness, f32 frac, f32 t, u32 col )
                                                                               { draw_border_progress( box, rounding, thickness, frac, t, col ); }
void gui_draw_inset_shadow( gui_rect_t box, f32 depth, u32 col ) { draw_inset_shadow( box, depth, col ); }
void gui_draw_stripes( gui_rect_t box, f32 spacing, f32 thickness, f32 angle, u32 col ) { draw_stripes( box, spacing, thickness, angle, col ); }
void gui_draw_shadow  ( gui_rect_t box, f32 spread, u32 col )             { draw_shadow( box, spread, col ); }
void gui_draw_glow    ( gui_rect_t box, f32 spread, u32 col )             { draw_glow( box, spread, col ); }
void gui_draw_drop_shadow( gui_rect_t box, f32 spread, f32 off_x, f32 off_y, u32 col )
                                                                               { draw_drop_shadow( box, spread, off_x, off_y, col ); }
void gui_draw_pulse   ( gui_rect_t box, f32 rate, f32 depth, f32 phase, u32 col ) { draw_pulse( box, rate, depth, phase, col ); }

/* text effects + decorations */
void gui_draw_text_outline( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline )
                                                                               { draw_text_outline( x, y, str, col_text, col_outline ); }
void gui_draw_text_shadow( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy )
                                                                               { draw_text_shadow( x, y, str, col_text, col_shadow, dx, dy ); }
void gui_draw_grip( gui_rect_t box, u32 col )                            { draw_grip_dots( box, col ); }
void gui_draw_dot_grid( gui_rect_t at, u32 nx, u32 ny, f32 pitch_x, f32 pitch_y, f32 size, u32 col )
                                                                               { draw_dot_grid( at, nx, ny, pitch_x, pitch_y, size, col ); }
void gui_draw_ticks( gui_rect_t bar, u32 n, f32 thickness, f32 len, bool vertical, u32 col )
                                                                               { draw_ticks( bar, n, thickness, len, vertical, col ); }
void gui_draw_dot_spinner( gui_rect_t box, u32 n, f32 dot, f32 rate, u32 col )
                                                                               { draw_dot_spinner( box, n, dot, rate, col ); }
void gui_draw_dial_ticks( gui_rect_t box, u32 n, f32 thickness, f32 len, f32 rate, u32 col )
                                                                               { draw_dial_ticks( box, n, thickness, len, rate, col ); }
void gui_draw_spinner( gui_rect_t box, f32 rate, f32 thickness, u32 col ) { draw_spinner( box, rate, thickness, col ); }
void gui_draw_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col ) { draw_progress_arc( cx, cy, r, frac, thickness, col ); }

// clang-format on
/*============================================================================================*/
