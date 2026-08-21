/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess_sdf.c -- the rounded-box SDF family.

    Part 4 of 7 of the CPU-side quad-record builder (see gui_build_tess_state.c for the family
    overview).  Holds tess_fx_box_core (the one surface every rounded shape in the library goes
    through, plus tess_fx_aux_t, its op-specific extras), the uniform-radius entry tess_fx_box,
    the repetition lattices built on it (tess_repeat_box, tess_repeat_polar), and the two
    triangle-lane shapes tess_triangle / tess_bezier.

    Included right after gui_build_tess_sprite.c (needs tess_quad_push, tess_snap_px).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    The SDF surface -- every rounded shape, in ONE quad.

    The CPU emits a covering quad and the fragment shader resolves the boundary exactly (gui.h,
    the effect band).  The covering is grown past the box by the falloff pad (the SKIRT rule) so
    a feathered edge -- and a shadow's whole soft skirt -- has somewhere to land; the BOUNDARY
    still sits exactly on the authored rect.

    A wide BAND/CUT/INSET surface rasterizes its interior at zero coverage (the fields early-out
    cheaply); the hole-carving the old vertex path did for those has no home in a one-rectangle
    record, and UI fill is nowhere near bound.

    Every SDF surface samples the same atlas as everything else and names its shape through a
    STYLE record, so it merges into whatever GPU command is already open: a soft shadow behind a
    panel costs a batch split of zero.
==============================================================================================*/

/* Emit one SDF surface.  `r4` is the corner radius PER QUADRANT, in the tessellation order
   top-left, top-right, bottom-right, bottom-left (tess_fx_box passes four copies of one radius;
   only tess_round_rect_ex passes four different ones), and `feather` the total width of the falloff
   band straddling the boundary (0 = hard edge); both are always read.  The remaining parameters are
   OP-SPECIFIC, mirroring the record's own re-partitioning -- `border` is the border width under
   GUI_OP_BAND, `rate`/`depth` the wave under GUI_OP_PULSE, and each ignores the other's.
   UVs span the AUTHORED box and are clamped over the grown skirt, so a textured rounded quad cannot
   bleed into its atlas neighbour where the coverage has already faded to nothing.

   The surface is always GUI_FX_BOX; which of the four ops it carries comes in on ambient state
   (s_tess.cur_ops), set by the caller BEFORE this runs.  GUI_OP_CUT and GUI_OP_INSET take no
   parameter of their own -- they read radius and feather exactly as a plain fill does.

   Per-corner radii are FREE: all four ride the record and the fragment picks the one its own
   quadrant wants, so the geometry does not change at all.

   Why neighbouring quadrants cannot seam, which is the part that has to be true for any of this to
   work.  Each quadrant measures from its OWN radius, so the obvious worry is that the two sides of
   a shared centre line disagree.  They cannot.  Take the horizontal one (local.y = 0): q.y is
   r - hy, and r is clamped to lim <= hy, so q.y <= 0 for every radius.  The y term therefore drops
   out of both branches of the field and what remains is

       d = max( |local.x| - hx, -hy )

   in which r has cancelled.  The same holds on the vertical centre line.  The selection lines are
   precisely where the corner radius stops contributing, so the two sides agree EXACTLY -- not
   approximately, and not merely because the interior saturates.  That was the load-bearing claim
   when the quadrants were separate QUADS and it is the same claim now that they are separate
   BRANCHES of one fragment, which is why the shape survived the fold moving. */
/* `rot` turns the whole surface about the box CENTRE (radians, screen space; 0 = the common
   axis-aligned path).  Only the corner POSITIONS rotate; the fragment un-rotates by the same pair
   out of the record to recover its box-local coordinate.  The UVs are computed from the UNROTATED
   position first, so a textured rotated box still maps its picture across the authored rect and
   clamps over the skirt exactly as the upright one does. */
/*----------------------------------------------------------------------------------------------
    tess_fx_aux_t -- the two extras a box surface can carry, absent from every plain fill.

    One pointer rather than four more parameters, because that is what they are: a rarely-taken
    branch off a call that already states sixteen things.  Both are read only when the op that owns
    them is set, the same rule `border` and `rate`/`depth` follow.
----------------------------------------------------------------------------------------------*/
typedef struct
{
    u32 grad_col;         // GUI_OP_GRAD: the ramp's far colour
    f32 grad_ang;         // GUI_OP_GRAD: axis, radians, box-local, 0 points +x (linear ramp only)
    f32 grad_mid;         // GUI_OP_GRAD: midpoint bend, already the exponent (0 = linear)
    f32 cut_dx, cut_dy;   // GUI_OP_CUT: the cut boundary's centre, offset from this shape's
    f32 anim_rate;        // CYCLES/SEC, for every animating op -- one unit, whatever a cycle
                          //   means to the op reading it (gui.h row 5)
    f32 anim_phase;       // CYCLES, the static offset that staggers same-rate elements
    u32 anim_curve;       // gui_curve_t: what the phase does between its endpoints
    f32 anim_param;       // the curve's own parameter -- exponent, step count, duty
    f32 dash_period;      // GUI_OP_DASH: px per on+off cycle, already snapped to the perimeter
    f32 dash_duty;        // GUI_OP_DASH: on-fraction of the period
    f32 dash_scroll;      // GUI_OP_DASH: periods slid per cycle -- 1 marches, 0 pins to the shape

    /* GUI_OP_REPEAT: the lattice, sharing row 6 with the dash above (gui.h).  The pitch is
       centre-to-centre and the cell is HALF the copy's size, which is the form the fragment folds
       in.  The count is not here because it is not stored -- see tess_repeat_box.
       GUI_OP_REPEAT_POLAR reads the first two lanes as the copy COUNT and the orbit radius
       instead; its count IS stored, since a circle has no extent to recover one from. */
    f32 rep_pitch_x, rep_pitch_y;
    f32 rep_cell_hx, rep_cell_hy;

    /* GUI_OP_CUT_SHAPE: the subtracted box's own geometry, row 6's fourth claimant.  Its centre
       rides cut_dx/cut_dy above -- the same vector GUI_OP_CUT offsets its flush cut by. */
    f32 cut_hx, cut_hy;
    f32 cut_r, cut_aa;

} tess_fx_aux_t;

/* `aux` NULL is a plain fill with neither extra -- almost every caller. */
static void
tess_fx_box_core( f32 x, f32 y, f32 w, f32 h, const f32* r4,
                  f32 feather, f32 border, f32 rate, f32 depth, f32 rot,
                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
                  const tess_fx_aux_t* aux )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    /* Clamp what is GEOMETRICALLY meaningless, and only that.  The long list that used to live
       here was the packed word's doing: every field had a fixed-point ceiling, and the geometry
       below is built from the same numbers, so a value the fragment could not see would leave the
       quad describing a different shape than the one it resolved.  The record has no
       ceilings, so what remains is the one bound that is about the SHAPE rather than the storage
       -- a corner radius past half the short side is a capsule -- plus the negatives, which are
       nonsense in every field. */
    f32 hx = w * 0.5f, hy = h * 0.5f;
    f32 lim = ( hx < hy ) ? hx : hy;

    f32 rq[ 4 ];
    f32 rmin, rmax;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 r = r4[ i ];
        if ( r > lim ) r = lim;                   /* a radius past half the short side is a capsule */
        if ( r < 0.0f ) r = 0.0f;
        rq[ i ] = r;
    }
    rmin = rmax = rq[ 0 ];
    for ( u32 i = 1; i < 4; ++i )
    {
        if ( rq[ i ] < rmin ) rmin = rq[ i ];
        if ( rq[ i ] > rmax ) rmax = rq[ i ];
    }
    if ( feather < 0.0f ) feather = 0.0f;
    if ( border  < 0.0f ) border  = 0.0f;
    if ( rate    < 0.0f ) rate  = 0.0f;
    if ( depth   < 0.0f ) depth = 0.0f;
    if ( depth   > 1.0f ) depth = 1.0f;   /* a fraction, not a pixel count -- a real upper bound */

    /* Grid-snap the origin like tess_rect_filled -- UNLESS the shape is a circle.
       Snapping exists to keep STRAIGHT edges crisp, and it is derived rather than passed in
       because the condition is a property of the shape, not of the caller: a shape has no straight
       edge in either axis exactly when it is square and its radius reached the half-extent.  That
       is a disc, and it is also a circular RING -- so both fall out of one test, which is what
       keeps them aligned.  It matters that they agree: a filled disc and a ring drawn at the same
       centre would otherwise sit up to half a pixel apart, and concentric marks are precisely how
       these get used.
       Snapping a circle is not merely pointless but harmful.  Its origin is (centre - r), so
       snapping quantizes the CENTRE, and a small dot animating along a path steps instead of
       gliding.  A pill (w != h, r == the short half-extent) still snaps, correctly -- it does have
       two straight edges.  With per-corner radii the test reads rMIN: a shape is a disc only when
       EVERY corner reached the limit, and one square corner is a straight edge worth snapping.
       A ROTATED box never snaps: it has no axis-aligned edge to keep crisp, and quantizing its
       centre is the animated-dot mistake again (tess_quad_xf's rule). */
    if ( rot == 0.0f && !( hx == hy && rmin >= lim ) )
    {
        x = tess_snap_px( x );
        y = tess_snap_px( y );
    }

    /* tex_idx 0 = solid-color convention, same as tess_rect_filled: GUI_OP_SELF, no texel
       consulted at all. */
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        s_tess.cur_ops |= GUI_OP_SELF;
        u0 = v0 = u1 = v1 = 0.0f;
    }

    f32 cx  = x + hx,   cy  = y + hy;
    f32 rcs = 1.0f, rsn = 0.0f;                   /* the rotation, computed once per shape */
    if ( rot != 0.0f ) { rcs = cosf( rot ); rsn = sinf( rot ); }

    /* The style: the AUTHORED shape, after the clamps and the grid snap above.  The falloff
       skirt is a rasterization detail the vertex stage grows the covering by (GUI_QUAD_RULE_
       SKIRT) -- the field the fragment resolves is measured from the real boundary, so the
       record states that one.  All four radii travel. */
    s_tess.cur_prim.field   = (u32)GUI_FX_BOX;
    s_tess.cur_prim.r_tl    = rq[ 0 ];
    s_tess.cur_prim.r_tr    = rq[ 1 ];
    s_tess.cur_prim.r_br    = rq[ 2 ];
    s_tess.cur_prim.r_bl    = rq[ 3 ];
    s_tess.cur_prim.feather = feather;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & ( GUI_OP_BAND | GUI_OP_FRAME ) ) ? border : 0.0f;
    s_tess.cur_rot_c = rcs;
    s_tess.cur_rot_s = rsn;
    /* The pulse states only its DEPTH as a parameter of its own; its rate joins the shared
       animation clock, which is what lets a curve authored once shape the breath, the spin and
       the marching ants alike. */
    s_tess.cur_prim.param_a = ( s_tess.cur_ops & GUI_OP_PULSE ) ? depth : 0.0f;
    if ( s_tess.cur_ops & GUI_OP_PULSE )
        s_tess.cur_prim.anim_rate = rate;

    /* The corner profile -- ambient over the command, like the ops, and applied only where there
       is a corner to profile: a square box has no arc to reshape, and leaving the lane at zero is
       what keeps square fills deduping onto one record. */
    s_tess.cur_prim.corner_pow = ( rmax > 0.0f ) ? s_tess.cur_corner_pow : 0.0f;

    /* GUI_OP_GLOW's dropoff, derived from the reach the caller already stated as the feather --
       there is no second parameter, because the distance the glow travels and the distance the
       covering has to grow by are the same distance.  ln(255) puts the falloff under one 8-bit
       step at half the feather, which is where the vertex stage's SKIRT pad ends: the halo fades
       out exactly at the edge of the quad carrying it, rather than being cut off inside it. */
    if ( s_tess.cur_ops & GUI_OP_GLOW )
        s_tess.cur_prim.glow_k = 5.5413f / fmaxf( feather * 0.5f, 0.5f );

    /* DITHER, derived rather than asked for: a wide falloff and a colour ramp are the two shapes
       that band on an 8-bit target, and half a step of screen noise is invisible everywhere else
       it could apply.  The 1 px AA feather stays clean -- there is no ramp to band. */
    if ( feather > 2.0f || ( s_tess.cur_ops & GUI_OP_GRAD ) )
        s_tess.cur_ops |= GUI_OP_DITHER;

    /* The animation lane and the perimeter dash, written only under the op that reads each --
       the zero-when-unused rule that keeps plain fills deduping onto one record. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_DASH ) )
    {
        s_tess.cur_prim.dash_period = aux->dash_period;
        s_tess.cur_prim.dash_duty   = aux->dash_duty;
        s_tess.cur_prim.dash_scroll = aux->dash_scroll;
    }

    /* The repetition ops read the SAME row under different names (gui.h, row 6).  Writing them
       here, beside the dash they share with, is what keeps "at most one of them" visible at the one
       place any of them can be written. */
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_REPEAT | GUI_OP_REPEAT_POLAR ) ) )
    {
        s_tess.cur_prim.dash_period = aux->rep_pitch_x;
        s_tess.cur_prim.dash_duty   = aux->rep_pitch_y;
        s_tess.cur_prim.dash_scroll = aux->rep_cell_hx;
        s_tess.cur_prim.reserved_c  = aux->rep_cell_hy;
    }
    if ( aux && ( s_tess.cur_ops & GUI_OP_CUT_SHAPE ) )
    {
        s_tess.cur_prim.dash_period = aux->cut_hx;
        s_tess.cur_prim.dash_duty   = aux->cut_hy;
        s_tess.cur_prim.dash_scroll = aux->cut_r;
        s_tess.cur_prim.reserved_c  = aux->cut_aa;
    }
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_DASH | GUI_OP_SPIN
                                   | GUI_OP_GRAD_CELL | GUI_OP_CELL_FILL ) ) )
        s_tess.cur_prim.anim_rate = aux->anim_rate;
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_DASH | GUI_OP_PULSE | GUI_OP_SPIN
                                   | GUI_OP_GRAD_CELL | GUI_OP_CELL_FILL ) ) )
    {
        s_tess.cur_prim.anim_curve = aux->anim_curve;
        s_tess.cur_prim.anim_param = aux->anim_param;
        s_tess.cur_phase = aux->anim_phase;
    }

    /* GUI_OP_FRAME's border colour does NOT land here -- it rides the quad (cur_col_border, set by
       the dispatcher before this call), so an animated border never adds a style record.  This
       record's own col_b is the SHAPE's second colour (GRAD, CHECKER, TEXT_EDGE); the
       two are different lanes on purpose.  See gui_fx_t.col_border (gui.h) and the dispatch of
       GUI_CMD_FRAME below. */

    /* GUI_OP_GRAD -- the ramp's far colour and its axis, stored as a UNIT direction.  The
       fragment divides by the extent the shape spans along it, recovered from the placement it
       already holds.  Storing the direction rather than direction-over-extent is what lets ONE
       record serve every size: the same ramp on a 40 px chip and a 400 px panel used to be two
       records, because the divisor was baked in here.  Linear and conic now store the same
       thing, which is also one branch fewer. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_GRAD ) )
    {
        s_tess.cur_prim.col_b    = aux->grad_col;
        s_tess.cur_prim.grad_mid = aux->grad_mid;

        /* A radial ramp has no axis, and neither does the per-copy one, so both stay ZERO rather
           than carrying an angle the fragment will not read -- otherwise two identical fills
           authored at different angles take two records for no reason (tess_prim_local memos on
           the record's bytes). */
        if ( !( s_tess.cur_ops & ( GUI_OP_GRAD_RADIAL | GUI_OP_GRAD_CELL ) ) )
        {
            s_tess.cur_prim.grad_x = cosf( aux->grad_ang );
            s_tess.cur_prim.grad_y = sinf( aux->grad_ang );
        }
    }

    /* GUI_OP_CUT -- where the cut boundary sits.  Zero is the shape cutting itself, which is every
       caller that wants a shadow cast straight down onto the ground under its subject; a non-zero
       offset is the DIRECTIONAL cast, the falloff measured from this outline while the hole is
       taken against the caster's. */
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_CUT | GUI_OP_CUT_SHAPE ) ) )
    {
        /* `+ 0.0f` folds NEGATIVE ZERO onto positive.  A caller that negates an offset it was
           handed hands one down for free (draw_push_skirt passes -ox), and -0.0f compares equal to
           0.0f while hashing and memcmp-ing as a different record -- so the un-offset cut would
           quietly take a second style entry that draws exactly the same shape. */
        s_tess.cur_prim.cut_dx = aux->cut_dx + 0.0f;
        s_tess.cur_prim.cut_dy = aux->cut_dy + 0.0f;
    }

    /* The COVERING: one quad, the shape's true extents under the SKIRT rule (the vertex stage
       grows them by the style's feather pad).  The old vertex path carved an interior hole out
       of wide BAND/CUT/INSET surfaces; a record stores one rectangle, so the interior rasterizes
       at zero coverage instead -- the fields early-out cheaply and UI fill is nowhere near
       bound.  The uv rect is the authored span; the vertex stage scales it over the skirt and
       clamps at the corners, so a textured rounded quad shows its picture at authored size. */
    tess_quad_push( cx, cy, hx, hy, GUI_QUAD_RULE_SKIRT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr,
                    GUI_GLYPH_ID_NONE );
}

/*----------------------------------------------------------------------------------------------
    tess_repeat_box -- nx by ny copies of one rounded cell, from ONE quad.

    The quad spans the whole set and the fragment folds its coordinate into a cell (GUI_OP_REPEAT),
    so the copy count costs nothing: a 3x3 grip and a 40-tick ruler are the same one record and the
    same one quad.

    THIS FUNCTION OWNS THE SIZING CONTRACT the fragment decodes against.  The set's half-extent must
    be exactly (n-1)/2 pitches plus one cell on each axis, because that is what the count is
    recovered from -- state the quad any other way and the lattice repeats the wrong number of
    times.  Deriving the extent here rather than taking a rect from the caller is what makes that
    unbreakable: there is no rect to get wrong.

    (cx, cy) is the SET's centre.  The pitch is floored just above the cell so copies can never
    touch, which the recovery also depends on.
----------------------------------------------------------------------------------------------*/

static void
tess_repeat_box( f32 cx, f32 cy, u32 nx, u32 ny, f32 pitch_x, f32 pitch_y,
                 f32 cell_w, f32 cell_h, f32 rounding, u32 abgr, u32 grad_col, f32 fill )
{
    if ( nx == 0u || ny == 0u || cell_w <= 0.0f || cell_h <= 0.0f )
        return;

    f32 chx = cell_w * 0.5f, chy = cell_h * 0.5f;

    /* Copies that touch would blur two cells into one shape AND break the count recovery, which
       divides the span by the pitch.  A hair over twice the half-extent keeps both honest. */
    f32 px = ( pitch_x > cell_w ) ? pitch_x : cell_w + 1.0f;
    f32 py = ( pitch_y > cell_h ) ? pitch_y : cell_h + 1.0f;

    /* The half-span of copy CENTRES, then the set's own half-extent. */
    f32 spanx = (f32)( nx - 1u ) * 0.5f * px;
    f32 spany = (f32)( ny - 1u ) * 0.5f * py;
    f32 hx    = spanx + chx;
    f32 hy    = spany + chy;

    /* A radius past half the cell's short side is a pill end; past that it is nothing the cell can
       be.  Clamped against the CELL, not the set, since the cell is the shape. */
    f32 lim = ( chx < chy ) ? chx : chy;
    if ( rounding > lim )  rounding = lim;
    if ( rounding < 0.0f ) rounding = 0.0f;

    tess_fx_aux_t aux = { 0 };
    aux.rep_pitch_x = px;
    aux.rep_pitch_y = py;
    aux.rep_cell_hx = chx;
    aux.rep_cell_hy = chy;

    s_tess.cur_ops |= GUI_OP_REPEAT;

    /* Per-copy variation, both static forms of the animation clock: the ramp toward grad_col
       (GUI_OP_GRAD_CELL) and the lit fraction (GUI_OP_CELL_FILL), whose threshold rides the
       instance phase at rate 0.  The phase lane wraps at 1, so a full meter states itself just
       under it -- every copy's slot sits below 0.9999 at any count the lattice can hold. */
    if ( grad_col != abgr )
    {
        s_tess.cur_ops |= GUI_OP_GRAD | GUI_OP_GRAD_CELL;
        aux.grad_col = grad_col;
    }
    if ( fill >= 0.0f )
    {
        s_tess.cur_ops |= GUI_OP_CELL_FILL;
        aux.anim_phase = ( fill < 0.9999f ) ? fill : 0.9999f;
    }

    /* Through the ordinary box path so the cell gets the same clamps, the same solid-fill
       convention and the same corner profile every other rounded shape does.  It states the SET's
       rect; the record's cell extent is what the field actually measures against. */
    const f32 r4[ 4 ] = { rounding, rounding, rounding, rounding };
    tess_fx_box_core( cx - hx, cy - hy, hx * 2.0f, hy * 2.0f, r4, TESS_FX_AA, 0.0f,
                      0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, abgr, &aux );
}

/*----------------------------------------------------------------------------------------------
    tess_repeat_polar -- n copies of one cell on a circle, from ONE quad.

    The angular twin of tess_repeat_box, and the reason the pair is worth having: at `rate` > 0 the
    ring turns on the SHADER CLOCK (GUI_OP_SPIN, composed upstream of the fold), so a rotating dot
    spinner is one quad whose command bytes never change -- it re-tessellates nothing while it runs.
    The caller still presents frames with request_redraw, the draw_pulse contract.

    Unlike the linear fold the count is STORED: a circle has no extent to recover one from, and the
    orbit takes the lane the second pitch would have used.

    The cell's frame turns with its angular position, so a WIDE cell reads as a dial tick pointing
    outward and a square one as a dot.  That is the only difference between the two things this
    draws.
----------------------------------------------------------------------------------------------*/

static void
tess_repeat_polar( f32 cx, f32 cy, u32 n, f32 orbit, f32 cell_w, f32 cell_h,
                   f32 rounding, f32 rate, f32 phase, u32 curve, f32 curve_param,
                   u32 abgr, u32 grad_col )
{
    if ( n == 0u || cell_w <= 0.0f || cell_h <= 0.0f || orbit <= 0.0f )
        return;

    f32 chx = cell_w * 0.5f, chy = cell_h * 0.5f;

    /* The set's half-extent.  A cell sits at distance `orbit` in some direction, so the furthest
       it reaches on either axis is orbit + that axis' half-extent -- tight at the four cardinal
       angles and conservative everywhere between. */
    f32 hx = orbit + chx;
    f32 hy = orbit + chy;

    f32 lim = ( chx < chy ) ? chx : chy;
    if ( rounding > lim )  rounding = lim;
    if ( rounding < 0.0f ) rounding = 0.0f;

    tess_fx_aux_t aux = { 0 };
    aux.rep_pitch_x = (f32)n;      /* the count, where the linear fold states a pitch */
    aux.rep_pitch_y = orbit;
    aux.rep_cell_hx = chx;
    aux.rep_cell_hy = chy;

    s_tess.cur_ops |= GUI_OP_REPEAT_POLAR;

    /* The ramp toward grad_col, by copy (GUI_OP_GRAD_CELL).  Under a rate it is what the clock
       drives INSTEAD of the frame: the dots stay put and the bright head marches around them,
       fading behind itself -- the tail spinner.  SPIN would move the copies themselves, and the
       two readings of one rate compose into a double crawl, so a ring takes one or the other. */
    if ( grad_col != abgr )
    {
        s_tess.cur_ops |= GUI_OP_GRAD | GUI_OP_GRAD_CELL;
        aux.grad_col   = grad_col;
        aux.anim_phase = phase;      /* at rate 0 the phase alone anchors the static ramp */
    }

    /* SPIN turns prim_frame, which is upstream of the angular fold, so the whole ring rotates as
       one rigid body.  With CURVE_STAIR at `n` steps it advances exactly one copy per step -- the
       mechanical clock-hand spinner, from the same record as the smooth one. */
    if ( rate > 0.0f )
    {
        if ( grad_col == abgr )
            s_tess.cur_ops |= GUI_OP_SPIN;
        aux.anim_rate    = rate;
        aux.anim_phase   = phase;
        aux.anim_curve   = curve;
        aux.anim_param   = curve_param;
    }

    const f32 r4[ 4 ] = { rounding, rounding, rounding, rounding };
    tess_fx_box_core( cx - hx, cy - hy, hx * 2.0f, hy * 2.0f, r4, TESS_FX_AA, 0.0f,
                      0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, abgr, &aux );
}

/*----------------------------------------------------------------------------------------------
    tess_box_cut -- a rounded box with a SECOND rounded box carved out of it (GUI_OP_CUT_SHAPE).

    Subtraction is the one composition more quads cannot build -- blending only ever adds ink --
    so it is the one that has to ride the record.  The cut's own geometry takes row 6 (the per-op
    row) and its centre the cut vector; the carved edge antialiases through its own aa band, so
    the notch reads as a boundary rather than a scissor bite.
----------------------------------------------------------------------------------------------*/

static void
tess_box_cut( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 dx, f32 dy,
              f32 cut_w, f32 cut_h, f32 cut_r, f32 cut_aa, u32 abgr )
{
    f32 chx = cut_w * 0.5f, chy = cut_h * 0.5f;

    /* A degenerate cut carves nothing; the fill is what the caller stated, so draw that. */
    if ( chx > 0.0f && chy > 0.0f )
    {
        f32 lim = ( chx < chy ) ? chx : chy;
        if ( cut_r > lim )  cut_r = lim;
        if ( cut_r < 0.0f ) cut_r = 0.0f;

        tess_fx_aux_t aux = { 0 };
        aux.cut_dx = dx;
        aux.cut_dy = dy;
        aux.cut_hx = chx;
        aux.cut_hy = chy;
        aux.cut_r  = cut_r;
        aux.cut_aa = ( cut_aa > TESS_FX_AA ) ? cut_aa : TESS_FX_AA;

        s_tess.cur_ops |= GUI_OP_CUT_SHAPE;

        const f32 r4[ 4 ] = { rounding, rounding, rounding, rounding };
        tess_fx_box_core( x, y, w, h, r4, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                          0, 0, 1, 1, 0, abgr, &aux );
        return;
    }

    const f32 r4[ 4 ] = { rounding, rounding, rounding, rounding };
    tess_fx_box_core( x, y, w, h, r4, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                      0, 0, 1, 1, 0, abgr, NULL );
}

/* The uniform-radius entry every rounded shape in the library goes through.  Four copies of one
   radius is not a workaround -- it is the honest statement that a rounded rect is the special case
   of a per-corner one, and it keeps a single tessellator for both. */
static void
tess_fx_box( f32 x, f32 y, f32 w, f32 h, f32 r, f32 feather, f32 border, f32 rate, f32 depth,
             f32 rot, f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
             const tess_fx_aux_t* aux )
{
    const f32 r4[ 4 ] = { r, r, r, r };
    tess_fx_box_core( x, y, w, h, r4, feather, border, rate, depth, rot,
                      u0, v0, u1, v1, tex_idx, abgr, aux );
}

/* TESS_FX_AA -- the default 1 px antialiasing band -- lives in gui_render.h now: the emit side
   bakes it into commands (a pulse's feather) as well. */

/* Tessellate a solid triangle into s_tess: the GUI_FX_TRI field -- one quad over the bbox,
   three points about its centre in the style's radius + param lanes.  Centre-relative points
   are what lets repeated arrow glyphs share one style; the edges antialias through the shared
   feather, which the old rasterized triangle never had. */
static void
tess_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    f32 lox = fminf( ax, fminf( bx, cx ) ), hix = fmaxf( ax, fmaxf( bx, cx ) );
    f32 loy = fminf( ay, fminf( by, cy ) ), hiy = fmaxf( ay, fmaxf( by, cy ) );
    if ( hix <= lox || hiy <= loy )
        return;
    f32 qx = ( lox + hix ) * 0.5f, qy = ( loy + hiy ) * 0.5f;

    s_tess.cur_ops         |= GUI_OP_SELF;
    s_tess.cur_prim.field   = (u32)GUI_FX_TRI;
    s_tess.cur_prim.r_tl    = ax - qx;
    s_tess.cur_prim.r_tr    = ay - qy;
    s_tess.cur_prim.r_br    = bx - qx;
    s_tess.cur_prim.r_bl    = by - qy;
    s_tess.cur_prim.param_a = cx - qx;
    s_tess.cur_prim.param_b = cy - qy;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_rot_c = 1.0f;
    tess_quad_push( qx, qy, ( hix - lox ) * 0.5f, ( hiy - loy ) * 0.5f,
                    GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}

/* Tessellate a stroked quadratic bezier into s_tess: the GUI_FX_BEZIER field -- one quad over
   the control points' bbox, thickened, with the three points about its centre riding the same
   lanes GUI_FX_TRI's points do.  Unlike the triangle this shape has real width, so the bbox is
   grown by the half-thickness (plus the AA pad) before the push -- GUI_QUAD_RULE_SKIRT only
   grows a quad by the feather, not by a shape's own size. */
static void
tess_bezier( f32 ax, f32 ay, f32 cx, f32 cy, f32 bx, f32 by, f32 thickness, u32 abgr )
{
    f32 lox = fminf( ax, fminf( bx, cx ) ), hix = fmaxf( ax, fmaxf( bx, cx ) );
    f32 loy = fminf( ay, fminf( by, cy ) ), hiy = fmaxf( ay, fmaxf( by, cy ) );
    if ( hix <= lox || hiy <= loy )
        return;
    f32 half = thickness * 0.5f + TESS_FX_AA;
    f32 qx   = ( lox + hix ) * 0.5f, qy = ( loy + hiy ) * 0.5f;

    s_tess.cur_ops         |= GUI_OP_SELF;
    s_tess.cur_prim.field   = (u32)GUI_FX_BEZIER;
    s_tess.cur_prim.r_tl    = ax - qx;
    s_tess.cur_prim.r_tr    = ay - qy;
    s_tess.cur_prim.r_br    = bx - qx;
    s_tess.cur_prim.r_bl    = by - qy;
    s_tess.cur_prim.param_a = cx - qx;
    s_tess.cur_prim.param_b = cy - qy;
    s_tess.cur_prim.border  = thickness * 0.5f;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_rot_c = 1.0f;
    tess_quad_push( qx, qy, ( hix - lox ) * 0.5f + half, ( hiy - loy ) * 0.5f + half,
                    GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}


// clang-format on
