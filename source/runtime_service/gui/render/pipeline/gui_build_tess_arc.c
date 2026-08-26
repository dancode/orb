/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess_arc.c -- circles, n-gons, sectors.

    Part 5 of 7 of the CPU-side quad-record builder (see gui_build_tess_state.c for the family
    overview).  Holds tess_circle_filled (a disc, built on tess_fx_box), tess_fx_ngon (regular
    polygons), tess_round_rect_ex (four independent corner radii plus a ramp), and tess_fx_arc
    (circular sectors -- ARC/PIE, stroked or filled, spun, dashed or colour-swept).  TESS_PI /
    TESS_HALF_PI / TESS_TAU are defined here and used by later files in this family.

    Included right after gui_build_tess_sdf.c (needs tess_fx_box, tess_fx_box_core, tess_quad_push).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    tess_circle_filled -- a disc, which is a rounded box whose radius reached its half-extent.

    There is no circle primitive anywhere in the pipeline and there does not need to be one:
    tess_fx_box clamps the corner radius to half the short side, so a SQUARE box asking for a
    radius of its own half-extent degenerates exactly to a disc -- same field, same one quad,
    same fragment, antialiased at any size.  The emit side agrees (draw_push_circle_filled emits
    GUI_CMD_RECT_FILL with rounding = r); this helper survives for tess_fx_arc's full-turn PIE
    route.

    NOT grid-snapped, and it does not have to ask: tess_fx_box derives it -- a square whose radius
    reached its half-extent has no straight edge for snapping to keep crisp, and quantizing a
    circle's centre is exactly what a small moving dot must not do.  A circular RING satisfies the
    same test, so the two stay aligned when drawn concentrically.
==============================================================================================*/

static void
tess_circle_filled( f32 pcx, f32 pcy, f32 r, u32 abgr )
{
    tess_fx_box( pcx - r, pcy - r, r * 2.0f, r * 2.0f,
                 r, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                 0, 0, 1, 1, 0, abgr, NULL );
}

/*==============================================================================================
    tess_fx_ngon -- a regular polygon (GUI_FX_NGON) in one quad.

    The polyline fan this replaces sampled up to 64 perimeter points and strung them through the
    ribbon; the field resolves the exact boundary at any size, and a stroked form is the same
    quad under GUI_OP_BAND -- set by the replay case before this runs, the ambient-ops rule every
    shape follows.  The record's row [2] re-partitions for the field: r_tl is the corner
    rounding, r_tr the side count, r_br the star inner-radius ratio -- 0 for the regular
    polygon, per the unused-lane dedup rule (gui.h).
==============================================================================================*/

static void
tess_fx_ngon( f32 pcx, f32 pcy, f32 r, u32 sides, f32 rot, f32 rounding,
              f32 border, f32 star, u32 abgr )
{
    if ( r <= 0.0f )
        return;

    s_tess.cur_prim.field   = (u32)GUI_FX_NGON;
    s_tess.cur_prim.r_tl    = rounding;
    s_tess.cur_prim.r_tr    = (f32)sides;
    s_tess.cur_prim.r_br    = star;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & GUI_OP_BAND ) ? border : 0.0f;
    s_tess.cur_rot_c = cosf( rot );
    s_tess.cur_rot_s = sinf( rot );

    /* The circumcircle covering under SKIRT: r on both axes, grown by the pad the vertex stage
       derives from the feather.  Rotation-safe -- a rotated square covering of a circle is
       still a covering. */
    s_tess.cur_ops |= GUI_OP_SELF;
    tess_quad_push( pcx, pcy, r, r, GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr,
                    GUI_GLYPH_ID_NONE );
}

/*==============================================================================================
    tess_round_rect_ex -- a fill or stroke whose four corners have four different radii.

    The tab, the notch, the asymmetric card: shapes that used to walk a per-corner perimeter (up to
    72 sampled points) and fan it into as many separate TRIANGLE commands, with a polygonal boundary
    and no antialiasing at all.  Here it is the ONE quad record a uniform rounded rect costs, and
    the boundary is exact, because all four radii ride the record and the fragment picks the one
    its own quadrant wants -- the radii are data, not geometry.  A stroke costs the same one quad:
    GUI_OP_BAND bends whatever scalar distance the field resolved to, which does not care how many
    radii shaped that distance.

        perimeter fan, 4 rounded corners   ~70 verts, 62 draw commands, aliased
        the field                          1 record,   1 draw command,   antialiased

    The RAMP rides the same record.  A linear one could be carried by the four corners instead --
    colour is affine along the axis and so is interpolation -- but only a linear one, and only
    approximately: the corners it would be evaluated at are the FALLOFF SKIRT's, a pixel or more
    outside the shape, so the ramp arrives stretched by however wide the skirt is.  Resolved in the
    fragment it spans the shape exactly, and the two ramps a rectangle's corners cannot describe at
    all -- radial and conic -- cost the same one branch.
==============================================================================================*/

static void
tess_round_rect_ex( f32 x, f32 y, f32 w, f32 h,
                    f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather, f32 border,
                    u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind, f32 grad_mid )
{
    /* Corner order: top-left, top-right, bottom-right, bottom-left -- the order gui_cmd_t
       .round_rect declares its radii in, and the order the record's r_tl/r_tr/r_br/r_bl carry
       them, which is what the fragment indexes by the sign of its own position.  feather below
       the standard AA band clamps up -- 0 means "crisp", never "hard-edged". */
    const f32 r4[ 4 ] = { rtl, rtr, rbr, rbl };

    if ( border > 0.0f )
        s_tess.cur_ops |= GUI_OP_BAND;

    /* Equal endpoints ARE a flat fill, so the op is left off rather than special-cased: a ramp
       between one colour and itself is that colour, and the fragment should not pay for it. */
    tess_fx_aux_t aux = { 0 };
    if ( col_b != abgr )
    {
        s_tess.cur_ops |= GUI_OP_GRAD;
        if ( grad_kind == (u32)GUI_GRAD_RADIAL ) s_tess.cur_ops |= GUI_OP_GRAD_RADIAL;
        if ( grad_kind == (u32)GUI_GRAD_CONIC  ) s_tess.cur_ops |= GUI_OP_GRAD_CONIC;
        aux.grad_col = col_b;
        aux.grad_ang = grad_ang;
        aux.grad_mid = grad_mid;
    }

    tess_fx_box_core( x, y, w, h, r4, ( feather > TESS_FX_AA ) ? feather : TESS_FX_AA,
                      border, 0.0f, 0.0f, 0.0f,
                      0, 0, 1, 1, 0, abgr, &aux );
}

/*==============================================================================================
    tess_round_frame_ex -- draw_push_frame's per-corner sibling: body + border band, one quad,
    four independent corner radii.

    GUI_OP_FRAME composites `abgr` (the body) and s_tess.cur_col_border (the band, set by the
    dispatcher before this runs -- see GUI_CMD_FRAME's own case) over the SAME field
    tess_round_rect_ex resolves; the per-corner radii cost nothing extra here either, for the
    identical reason.  No gradient lane -- see the round_frame_ex struct comment (gui.h). */

static void
tess_round_frame_ex( f32 x, f32 y, f32 w, f32 h,
                     f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 t, u32 abgr )
{
    const f32 r4[ 4 ] = { rtl, rtr, rbr, rbl };
    f32 rmax = rtl;
    if ( rtr > rmax ) rmax = rtr;
    if ( rbr > rmax ) rmax = rbr;
    if ( rbl > rmax ) rmax = rbl;

    s_tess.cur_ops |= GUI_OP_FRAME;
    tess_fx_box_core( x, y, w, h, r4, ( rmax > 0.0f ) ? TESS_FX_AA : 0.0f,
                      t, 0.0f, 0.0f, 0.0f,
                      0, 0, 1, 1, 0, abgr, NULL );
}

/*==============================================================================================
    tess_fx_arc -- a circular sector, stroked (ARC) or filled (PIE), in ONE quad.

    The last sampled curve in the library.  An arc used to be up to 66 points from cos/sin fed to
    the polyline ribbon (~130 vertices, and a visible polygon at small radii where sym_arc_segs
    gives a 10 px mark ten segments); a pie fanned the same points from the centre, which cost 65
    separate TRIANGLE commands -- 6% of the entire per-frame command budget for one shape.

        spinner, r = 24    ~90 verts,  1 draw cmd,  faceted, ribbon-AA
        pie,     r = 40    ~66 verts, 65 draw cmds, faceted, no AA
        the field          1 record,   1 draw cmd,  exact, antialiased

    A circular shape subtracts no half-extent: its effect coordinate is the raw signed offset from
    the centre, which is affine everywhere, so nothing has to fold at the vertex stage (gui.h).
    Keeping the sign is also the only reason an arc is expressible at all -- |p| would erase the
    angle.

    What the CPU does here is the per-shape work the fragment must not repeat: rotate the coordinate
    frame so the sector's bisector points +y.  That turns two absolute angles into one aperture (the
    shape is then symmetric about local x = 0, which the fragment folds itself) and it is paid once
    per record instead of once per pixel.  The matrix is a reflection and its own inverse, so the
    same two lines map local -> world here as map world -> local conceptually.

    The quad is the sector's bounding box IN THAT LOCAL FRAME, not the circle's: a 90-degree arc
    covers about a quarter of the disc's area, so the fragment cost tracks the shape rather than the
    circle it belongs to.  A full turn is not a sector at all and routes to the exact ring / disc
    primitives instead -- cheaper, and it sidesteps the aperture = pi degenerate.
==============================================================================================*/

#define TESS_PI       3.14159265358979f
#define TESS_HALF_PI  1.57079632679490f
#define TESS_TAU      6.28318530717959f

/* `mode` is GUI_FX_ARC or GUI_FX_PIE.  Every sector is self-sampled (GUI_OP_SELF): the fragment
   never reads a texel, so the atlas index the quad carries is only there to keep the bound slot
   valid.
   A non-zero `dash_turns` dashes the sector through GUI_OP_DASH, in period-turns and on-duty.
   A `grad_col` differing from `abgr` sweeps the colour toward it along the stroke
   (GUI_OP_GRAD_ALONG); passing `abgr` states no sweep -- a ramp between one colour and itself
   is that colour, the tess_round_rect_ex rule.
   `flat_caps` squares off a stroked sector's ends (GUI_OP_FLAT_CAP) instead of the round caps
   the field draws for free; ignored on a PIE, which has no caps. */
static void
tess_fx_arc( f32 pcx, f32 pcy, f32 r, f32 thickness, f32 a0, f32 a1,
             gui_fx_mode_t mode, u32 grad_col, f32 dash_turns, f32 dash_duty,
             f32 spin_rate, f32 spin_phase, u32 curve, f32 curve_param, u32 abgr,
             bool flat_caps )
{
    if ( r <= 0.0f )
        return;

    bool pie = ( mode == GUI_FX_PIE );

    /* Normalize the sweep so the bisector/aperture split below is always well formed.  A reversed
       range is the same sector drawn the other way round, which for a symmetric shape is the same
       sector.  (The gradient is NOT symmetric; its emit side pre-normalizes and swaps the colours,
       so by here every reversed range really is harmless.) */
    f32 sweep = a1 - a0;
    if ( sweep < 0.0f ) { f32 t = a0; a0 = a1; a1 = t; sweep = -sweep; }
    if ( sweep <= 0.0f )
        return;
    if ( sweep > TESS_TAU ) { sweep = TESS_TAU; a1 = a0 + TESS_TAU; }

    /* A full turn is not a sector, and the exact primitives are cheaper: a PIE is a disc, and an
       ARC is a closed ring, which is a BOX under GUI_OP_BAND whose interior the band carves away
       -- worth real fragments on a large one.  It is reachable: draw_progress_arc at 100% is
       exactly a full sweep.  The reroute used to be gated on the band fitting the packed `border`
       field, so a thick ring fell through to the sector formula (exact at aperture pi, it merely
       rasterizes the hole); the record has no such ceiling, so every full-turn ring takes the
       cheaper path now.
       A dashed or gradient sector never reroutes: the dash cut and the sweep both measure against
       the sector's own frame, which the exact ring does not build -- and at aperture pi the sector
       formula serves them exactly, so a closed dashed ring is this same one quad. */
    if ( sweep >= TESS_TAU && dash_turns <= 0.0f && grad_col == abgr )
    {
        if ( pie )
        {
            tess_circle_filled( pcx, pcy, r, abgr );
            return;
        }
        /* The same shape draw_circle's unfilled path asks for, measured from the OUTER boundary
           inward -- so the band still straddles r. */
        f32 outer = r + thickness * 0.5f;
        s_tess.cur_ops |= GUI_OP_BAND;
        tess_fx_box( pcx - outer, pcy - outer, outer * 2.0f, outer * 2.0f,
                     outer, TESS_FX_AA, thickness, 0.0f, 0.0f, 0.0f,
                     0, 0, 1, 1, 0, abgr, NULL );
        return;
    }

    f32 ra = r;
    f32 rb = pie ? 0.0f : thickness * 0.5f;
    if ( rb < 0.0f ) rb = 0.0f;

    f32 am = ( a0 + a1 ) * 0.5f;          /* the bisector, which becomes local +y */
    f32 ap = sweep * 0.5f;                /* the half-aperture measured from it   */
    f32 sm = sinf( am ), cm = cosf( am );
    f32 sa = sinf( ap ), ca = cosf( ap );

    /* The sector's bounding box in local space.  x is bounded by the widest point of the sweep
       (sin saturates at 1 once the aperture passes a quarter turn) plus the tube; y runs from the
       far edge of the sweep up to the bisector's own rim.  A PIE also contains its centre, which a
       narrow sweep's box would otherwise sit entirely above. */
    f32 pad  = TESS_FX_AA * 0.5f + 1.0f;
    f32 xext = ( ( ap >= TESS_HALF_PI ) ? ra : ra * sa ) + rb + pad;
    f32 ymax = ra + rb + pad;
    f32 ymin = ra * ca - rb - pad;
    if ( pie && ymin > -pad )
        ymin = -pad;

    /* A SPINNING sector sweeps the whole disc over time while its retained quad never moves,
       so the quad must cover every orientation the fragment will ever resolve -- the disc's own
       bounding box, not the sector's. */
    if ( spin_rate != 0.0f )
    {
        xext = ra + rb + pad;
        ymax = ra + rb + pad;
        ymin = -ymax;
    }

    /* The record.  (cm, sm) is the sector's own frame -- the bisector direction the local
       coordinate above is expressed in -- so it goes where every other field's turn goes. */
    s_tess.cur_prim.field   = (u32)mode;
    s_tess.cur_rot_c = cm;
    s_tess.cur_rot_s = sm;
    s_tess.cur_prim.param_a = ra;
    s_tess.cur_prim.param_b = rb;
    s_tess.cur_prim.param_c = ap;

    /* PIE has no caps to flatten -- its edges are already sharp radial cuts -- so the flag only
       ever reads on the stroked sector. */
    if ( !pie && flat_caps )
        s_tess.cur_ops |= GUI_OP_FLAT_CAP;

    /* GUI_OP_SPIN -- the whole frame (aperture, dashes, everything the record states) rotates at
       anim_rate turns/sec on pc.time.  The record is byte-identical every frame it runs, which is
       the point: the spinner joins the pulse in re-tessellating nothing. */
    if ( spin_rate != 0.0f )
    {
        s_tess.cur_ops |= GUI_OP_SPIN;
        s_tess.cur_prim.anim_rate  = spin_rate;
        s_tess.cur_phase = spin_phase;
    }

    /* A DASHED sector is the plain sector plus GUI_OP_DASH now -- the sector states arc-length as
       its boundary coordinate, which is the one axis the dash op cuts on whatever the shape.  The
       caller still speaks in turns, so convert once here: a period of `uvx` turns is that fraction
       of the full circumference at this radius.  The snap keeps whole cycles around the sweep, so
       a closed dashed ring meets itself exactly as the retired ARC_DASH field arranged. */
    if ( dash_turns > 0.0f )
    {
        f32 arc_len = sweep * ra;
        f32 period  = dash_turns * TESS_TAU * ra;
        f32 cycles  = ( period > 0.0f ) ? arc_len / period : 0.0f;
        if ( cycles >= 1.0f )
            period = arc_len / (f32)(i32)( cycles + 0.5f );

        s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_DASH;
        s_tess.cur_prim.dash_period = period;
        s_tess.cur_prim.dash_duty   = dash_duty;

        /* A spinning sector already carries its dashes around with it -- the boundary coordinate
           they are cut on is measured in the frame the spin rotates -- so the pattern is pinned
           to the shape rather than scrolled a second time along it.  A static sector has no
           rotation to ride, so there the clock is what moves the pattern at all. */
        s_tess.cur_prim.dash_scroll = ( spin_rate != 0.0f ) ? 0.0f : 1.0f;
    }

    /* The curve belongs to the clock, not to any one op that reads it, so it lands once here for
       whichever of the two the sector turned on. */
    if ( s_tess.cur_ops & ( GUI_OP_SPIN | GUI_OP_DASH ) )
    {
        s_tess.cur_prim.anim_curve = curve;
        s_tess.cur_prim.anim_param = curve_param;
    }

    /* The colour sweep: ramp the fill toward col_b along the arc-length coordinate the sector
       states -- the same axis the dash cuts on (GUI_OP_GRAD_ALONG). */
    if ( grad_col != abgr )
    {
        s_tess.cur_ops |= GUI_OP_GRAD | GUI_OP_GRAD_ALONG;
        s_tess.cur_prim.col_b = grad_col;
    }

    static const f32 lsx[ 4 ] = { -1.0f, 1.0f, 1.0f, -1.0f };
    static const u32 lsy[ 4 ] = {  0u,   0u,   1u,   1u   };

    /* The sector's frame is a REFLECTION, which the vertex stage's rotation cannot reproduce,
       so the covering goes out under the BBOX rule -- axis-aligned half-extents that reach
       every reflected corner from the SHAPE centre (the fragment's rotation origin).  The fold
       to the centre wastes the asymmetric slack a tight bbox would trim; sectors are small. */
    s_tess.cur_ops |= GUI_OP_SELF;
    f32 bhx = 0.0f, bhy = 0.0f;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 lx = lsx[ i ] * xext;
        f32 ly = lsy[ i ] ? ymax : ymin;
        f32 rx = -sm * lx + cm * ly;
        f32 ry =  cm * lx + sm * ly;
        if ( fabsf( rx ) > bhx ) bhx = fabsf( rx );
        if ( fabsf( ry ) > bhy ) bhy = fabsf( ry );
    }
    tess_quad_push( pcx, pcy, bhx, bhy, GUI_QUAD_RULE_BBOX,
                    0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}


// clang-format on
