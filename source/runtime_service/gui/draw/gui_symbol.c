/*==============================================================================================

    runtime_service/gui/draw/gui_symbol.c -- Symbol + shape render primitives.

    The frontend "render route" palette: the small glyph marks the chrome draws (arrows, check,
    bullet, close, pointer beak) plus the broader shape family editor / custom widgets reach for
    (frames, per-corner rounded rects, regular polygons, circles / rings / arcs / pie wedges,
    bezier curves, dashed lines, checker / hatch fills, gradients, soft shadows, outlined / shadowed
    text, resize grips, spinners, progress arcs).  This is the Dear ImGui Render* / AddXxx family.

    These compose the *backend* primitives (draw_push_triangle / _circle_filled / _rect_filled /
    _rect_outline / _text, gui_draw_line / gui_draw_polyline) into named marks -- they draw
    through the normal vertex pipeline, NOT the runtime icon atlas (gui_icon.c).  Two routes do
    the heavy lifting: a triangle fan (sym_fill_convex) fills any convex outline, and a closed / open
    polyline strokes it; arcs are sampled from cos / sin once per call.

    Most commands carry one abgr, but GUI_CMD_RECT_GRADIENT carries two and lets the GPU's
    per-vertex color interpolation blend them, so draw_gradient is an exact one-quad blend (not
    banded).  draw_shadow and the uniform-radius draw_round_rect hand their shape to the FRAGMENT
    (GUI_CMD_FX_BOX / a rounded rect command, both SDF surfaces -- see the effect band in gui.h):
    exact edges at any radius and softness, four quads, no batch split.  Only genuinely per-corner
    radii still walk a tessellated perimeter here.  Everything here is pixel-exact.

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

/* Dropdown-arrow glyph: a soft downward 'v' -- two open strokes, not a filled triangle (that reads
   as the collapse/expand glyph) -- fitted and centered in `box` (the toolbar split-button's
   affordance mark).  Scales off min(w,h) like draw_check_mark rather than off box.h alone like
   draw_arrow, so it drops cleanly into a narrow side column without overrunning the column width.
   Fixed DOWN orientation -- unlike draw_arrow it is not a general direction glyph and does not
   follow GUI_VAR_ARROW_SHAPE; the toolbar affordance stays this one mark regardless of theme. */
void
draw_dropdown_arrow( gui_rect_t box, u32 color )
{
    f32 sz = box.w < box.h ? box.w : box.h;
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( sz * 0.22f );          /* half-extent */
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
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( ( box.w < box.h ? box.w : box.h ) * 0.3f );
    if ( thickness < 1.0f ) thickness = 1.0f;

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
   surface -- one command, 16 vertices, an exact antialiased boundary at any radius -- because a
   quadrant quad already covers exactly one corner, so four radii cost four packed words and no
   extra geometry.  It used to fan the sampled perimeter into up to 62 separate TRIANGLE commands
   with a polygonal, unantialiased edge.  STROKED still walks the perimeter: an outline of four
   different radii is not a shape GUI_FX_RING can describe (its band is derived from one), and the
   closed antialiased polyline draws it correctly already. */
void
draw_round_rect_ex( gui_rect_t b, f32 rtl, f32 rtr, f32 rbr, f32 rbl,
                    bool filled, f32 thickness, u32 col )
{
    if ( filled )
    {
        draw_push_round_rect_ex( b.x, b.y, b.w, b.h, rtl, rtr, rbr, rbl, 0.0f, col );
        return;
    }
    gui_vec2_t pts[ 4 * 17 + 4 ];
    u32 n = round_rect_perimeter_ex( b, rtl, rtr, rbr, rbl, pts );
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
        sym_fill_convex( pts, sides, col );
    else
        gui_draw_polyline( pts, sides, thickness < 1.0f ? 1.0f : thickness, GUI_STROKE_CENTER, true, col );
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
   its own: GUI_FX_RING already paints a band `border` px wide lying INSIDE the boundary.  Two
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

    if ( thickness < 1.0f ) thickness = 1.0f;

    /* A band wider than the packed word's border field cannot be described to the fragment, so the
       heavy case keeps the polyline rather than silently drawing a thinner ring.  At 15.875 px this
       is a hoop, not an outline, and nothing in the library asks for one. */
    if ( thickness <= GUI_FX_BORDER_MAX )
    {
        f32 outer = r + thickness * 0.5f;
        /* draw_push_rect_outline takes its radius from the AMBIENT rounding, clamped to half the
           extent -- so asking for the full half-extent is how a square becomes a circle here. */
        f32 save = draw_rounding();
        draw_set_rounding( outer );
        draw_push_rect_outline( cx - outer, cy - outer, outer * 2.0f, outer * 2.0f,
                                thickness, col );
        draw_set_rounding( save );
        return;
    }

    u32 segs = sym_arc_segs( r, SYM_TAU );
    gui_vec2_t pts[ 64 ];
    for ( u32 i = 0; i < segs; ++i )
    {
        f32 a = SYM_TAU * ( (f32)i / (f32)segs );
        pts[ i ] = sv2( cx + cosf( a ) * r, cy + sinf( a ) * r );
    }
    gui_draw_polyline( pts, segs, thickness, GUI_STROKE_CENTER, true, col );
}

/* Stroked arc from a0 to a1 (radians) -- a spinner sweep, a knob track, a radial-menu rim.

   A distance field now, so the curve is exact at any radius and the cost is four vertices instead
   of the ~130 the sampled ribbon spent.  It matters most where these actually get used: sym_arc_segs
   gave a 12 px spinner about ten segments, so the "circle" it swept was a visible decagon. */
static void
draw_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col )
{
    if ( thickness < 1.0f ) thickness = 1.0f;

    /* A band wider than the packed word's tube field cannot be described to the fragment, so the
       heavy case keeps the polyline rather than silently drawing a thinner arc -- the same rule and
       the same reason as draw_circle's fat ring. */
    if ( thickness * 0.5f <= GUI_FX_ARC_TUBE_MAX )
    {
        draw_push_arc( cx, cy, r, thickness, a0, a1, col );
        return;
    }
    gui_vec2_t pts[ 66 ];
    u32 n = sym_arc( cx, cy, r, a0, a1, pts );
    gui_draw_polyline( pts, n, thickness, GUI_STROKE_CENTER, false, col );
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

/*==============================================================================================
    Patterned lines + fills
==============================================================================================*/

/* Checkerboard fill of `box` with `cell`-sized squares alternating col_a / col_b -- the classic
   transparency backdrop behind a color swatch.  Cell count is capped so a large area cannot flood
   the command list; partial edge cells are clamped to the box. */
void
draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b )
{
    if ( cell < 1.0f ) cell = 1.0f;
    u32 cols = (u32)ceilf( box.w / cell ), rows = (u32)ceilf( box.h / cell );
    if ( cols > 64 ) cols = 64;
    if ( rows > 64 ) rows = 64;

    /* Batched through the rect POOL, not one command per cell.  This is the exact caller
       draw_push_rect_list was added for: at the 64x64 clamp a checker is 4096 quads, four times
       the whole per-frame command budget in a normal build, so the one-command-per-cell form did
       not merely cost slots -- it silently ate the budget and every shape emitted after it in the
       frame vanished.  Chunked a row at a time so the scratch stays a fixed 64 entries and a full
       checker costs 64 commands instead of 4096. */
    gui_rect_col_t run[ 64 ];

    f32 save = draw_rounding();
    draw_set_rounding( 0.0f );
    for ( u32 yy = 0; yy < rows; ++yy )
    {
        u32 n = 0;
        for ( u32 xx = 0; xx < cols; ++xx )
        {
            f32 px = box.x + xx * cell, py = box.y + yy * cell;
            f32 cw = px + cell > box.x + box.w ? box.x + box.w - px : cell;
            f32 ch = py + cell > box.y + box.h ? box.y + box.h - py : cell;
            bool is_odd_cell = ( ( xx + yy ) & 1u ) != 0;
            run[ n++ ] = ( gui_rect_col_t ){ px, py, cw, ch, is_odd_cell ? col_b : col_a };
        }
        draw_push_rect_list( run, n );
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
void
draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal )
{
    draw_push_rect_gradient( box.x, box.y, box.w, box.h, col_a, col_b, horizontal );
}

/* Soft drop shadow / glow behind `box`, alpha falling off over `spread` px (popups, floating
   panels).  ONE SDF surface: the fragment shader resolves the falloff exactly, so this is no
   longer the six stacked rects that used to stand in for a gaussian -- and it costs four quads
   in whatever batch is already open rather than six commands of its own.  Honors the ambient
   rounding so it hugs a rounded panel.  Draw it before the panel body.

   `spread` reads as "how far the shadow reaches", so it is HALF the falloff band: the fill is
   solid until spread px inside the box and gone spread px outside it. */
static void
draw_shadow( gui_rect_t box, f32 spread, u32 col )
{
    draw_push_shadow( box.x, box.y, box.w, box.h, draw_rounding(), spread * 2.0f, col );
}

/* A rounded fill whose alpha breathes, evaluated in the FRAGMENT off the shared frame clock.
   The caller-visible difference from animating the color yourself is what it does NOT do: the
   command's bytes are identical every frame, so the window's retained geometry is never
   invalidated and the pulse re-tessellates nothing.  It is the cheap way to keep something
   asking for attention on a screen full of static widgets.

   `rate` is in Hz (quantized to 1/4 Hz) and `depth` the 0..1 fraction of alpha taken at the
   trough.  Honors the ambient rounding.  The frame still has to be presented -- the caller runs
   gui()->request_redraw() while the pulse is live (see GUI_FX_TIME_WRAP in gui.h). */
static void
draw_pulse( gui_rect_t box, f32 rate, f32 depth, u32 col )
{
    draw_push_pulse( box.x, box.y, box.w, box.h, draw_rounding(), rate, depth, col );
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
       backend's single rounded-rect command, which is an SDF surface (four quads, exact analytic
       AA).  Only genuinely asymmetric corners still need the perimeter walk, and they pay a
       tessellated arc and a polyline stroke for it. */
    bool equal_corners = ( r_tl == r_tr && r_tr == r_br && r_br == r_bl );
    if ( equal_corners )
    {
        f32 save = draw_rounding();
        draw_set_rounding( r_tl );
        if ( filled ) draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0, 0, 1, 1, 0, col );
        else          draw_push_rect_outline( box.x, box.y, box.w, box.h,
                                              thickness < 1.0f ? 1.0f : thickness, col );
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
    if ( thickness < 1.0f ) thickness = 1.0f;
    draw_push_arc_dashed( cx, cy, r, thickness, a0, a1, dash, gap, col );
}
void gui_draw_arc_gradient( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness,
                            u32 col_a, u32 col_b )
{
    if ( thickness < 1.0f ) thickness = 1.0f;
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
    draw_push_round_rect_ex( box.x, box.y, box.w, box.h, r_tl, r_tr, r_br, r_bl, feather, col );
}

/* curves */
void gui_draw_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col )
                                                                               { draw_bezier_quad( x0, y0, cx, cy, x1, y1, thickness, col ); }
void gui_draw_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col )
                                                                               { draw_bezier_cubic( x0, y0, c0x, c0y, c1x, c1y, x1, y1, thickness, col ); }

/* patterned lines + fills.  (gui_draw_dashed_line is the backend primitive in gui_emit_path.c;
   the vtable binds straight to it.) */
void gui_draw_checker ( gui_rect_t box, f32 cell, u32 col_a, u32 col_b )  { draw_checker( box, cell, col_a, col_b ); }
void gui_draw_hatch   ( gui_rect_t box, f32 spacing, f32 thickness, u32 col ) { draw_hatch( box, spacing, thickness, col ); }
void gui_draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal ) { draw_gradient( box, col_a, col_b, horizontal ); }
void gui_draw_shadow  ( gui_rect_t box, f32 spread, u32 col )             { draw_shadow( box, spread, col ); }
void gui_draw_pulse   ( gui_rect_t box, f32 rate, f32 depth, u32 col )    { draw_pulse( box, rate, depth, col ); }

/* text effects + decorations */
void gui_draw_text_outline( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline )
                                                                               { draw_text_outline( x, y, str, col_text, col_outline ); }
void gui_draw_text_shadow( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy )
                                                                               { draw_text_shadow( x, y, str, col_text, col_shadow, dx, dy ); }
void gui_draw_grip( gui_rect_t box, u32 col )                            { draw_grip_dots( box, col ); }
void gui_draw_spinner( gui_rect_t box, f32 t, f32 thickness, u32 col )    { draw_spinner( box, t, thickness, col ); }
void gui_draw_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col ) { draw_progress_arc( cx, cy, r, frac, thickness, col ); }

// clang-format on
/*============================================================================================*/
