/*==============================================================================================

    gui/render/pipeline/gui_emit_fx.c -- The SDF surface family.

    The pushes whose shape is resolved by the FRAGMENT off a prim record rather than geometry:
    the fx_box faces (shadow, skirt, glow, inset, pulse, rotated box), the per-corner rounded
    rect, the circular sectors and their dashed / gradient forms, the cell and lattice patterns,
    the regular polygon, the dashed and traced box outlines, and the two repeat lattices.

    What they have in common is that one quad covers any size and any count, and that the ones
    carrying a rate animate on the shader clock -- their bytes are identical every frame, so a
    pulse or a spinner re-tessellates nothing.  See gui_common.hlsli for the ops they stamp.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    draw_push_shadow / draw_push_pulse -- the two faces of GUI_CMD_FX_BOX, one body.

    A shadow is the surface with a WIDE feather: the falloff band straddles the shape's boundary
    (solid feather/2 inside the box, gone feather/2 outside), and because the effect travels per
    vertex it merges into whatever GPU batch is already open -- a shadow behind every floating
    panel costs no draw calls.

    draw_push_skirt is the same surface with its interior cut away (GUI_OP_CUT) -- identical
    outward falloff, nothing painted inside the boundary.  That is what a DROP shadow is: the core
    of a filled one can only ever be seen through the thing casting it, so on a translucent panel
    it reads as the panel dimming itself.  Cutting it also makes the tessellator's interior hole
    unconditional, taking a window-sized plate down to a band of quads around the frame.  Keep
    draw_push_shadow for a glow or halo that is MEANT to be seen through its subject.

    A pulse is the surface whose alpha breathes on pc.time in the FRAGMENT.  Geometrically it is
    a plain rounded fill, and that identity is the feature: the command's bytes never change, so
    its hash never changes, so the window's cached geometry stays valid and the pulse costs zero
    re-tessellation while it runs.  `rate` is in Hz,
    `depth` the 0..1 fraction of alpha removed at the trough.  The caller still owes one
    request_redraw per frame: the clock advancing is not what schedules a frame (GUI_FX_TIME_WRAP).
==============================================================================================*/

static void
draw_fx_box_cmd( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, u32 variant,
                 f32 rate, f32 depth, f32 phase, f32 rot, f32 cut_dx, f32 cut_dy,
                 f32 swell, f32 border, u32 abgr )
{
    /* Cull against the GROWN box: the falloff skirt is real geometry (feather/2 past the rect,
       plus the tessellator's pixel of slack), and a shadow whose box is just off screen still
       paints a band on it.  A swelling boundary reaches `swell` px further at full stretch.
       A rotated box culls against its rotated AABB -- computed here rather
       than approximated with the diagonal, because the exact box is four multiplies and the
       diagonal wrongly keeps every long thin rotated bar on screen. */

    /* A BAKED shape (the ambient draw_set_shape) replaces the analytic box, and the rect the caller
       stated is where its INK goes.  The quad has to be the PADDED box instead: the field's margin
       is what every border and glow travels through, and a quad stopping at the ink would leave the
       skirt clamping against the tenant's edge texel with nothing to reach into.

       The ink is aspect-FIT rather than stretched, the draw_icon_in rule -- and here it is not only
       taste: the fragment recovers its distance from a screen-space derivative, so a non-uniform
       scale makes the antialiasing band wider on one axis than the other and every effect measured
       in px stops being isotropic.

       Metrics rather than uvs, because these are the numbers a repack cannot move; the tenant's
       PLACEMENT is resolved in the tessellator (shape_uv). */
    gui_shape_id_t shape = s_draw.shape;
    if ( shape != GUI_SHAPE_NONE )
    {
        u32 ix, iy, iw, ih, fw, fh;
        if ( !shape_metrics( shape, &ix, &iy, &iw, &ih, &fw, &fh, NULL )
          || iw == 0 || ih == 0 )
            return;

        f32 sx = w / (f32)iw, sy = h / (f32)ih;
        f32 s  = ( sx < sy ) ? sx : sy;

        /* Centre the fitted ink in the rect the caller gave, then step back to the tenant origin
           by where the ink sits inside it -- so the padded box lands with the art exactly where an
           aspect-fit icon would have put it. */
        f32 ink_w = (f32)iw * s,   ink_h = (f32)ih * s;
        f32 ink_x = x + ( w - ink_w ) * 0.5f;
        f32 ink_y = y + ( h - ink_h ) * 0.5f;

        x = ink_x - (f32)ix * s;
        y = ink_y - (f32)iy * s;
        w = (f32)fw * s;
        h = (f32)fh * s;

        /* The sampled field is the shape; a corner radius on top of it would round the padded BOX,
           which is a rectangle the art does not fill. */
        rounding = 0.0f;
    }

    f32 pad = feather * 0.5f + 1.0f + fmaxf( swell, 0.0f );
    f32 bx = x, by = y, bw = w, bh = h;
    if ( rot != 0.0f )
    {
        f32 cs = cosf( rot ), sn = sinf( rot );
        f32 hx = w * 0.5f, hy = h * 0.5f;
        f32 ex = fabsf( hx * cs ) + fabsf( hy * sn );
        f32 ey = fabsf( hx * sn ) + fabsf( hy * cs );
        bx = x + hx - ex;  by = y + hy - ey;
        bw = ex * 2.0f;    bh = ey * 2.0f;
    }
    u32 col = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_FX_BOX, col, bx, by, bw, bh, pad );
    if ( !e )
        return;

    e->fx_box.x             = x;
    e->fx_box.y             = y;
    e->fx_box.w             = w;
    e->fx_box.h             = h;
    e->fx_box.rounding      = rounding;
    e->fx_box.corner_pow    = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    e->fx_box.feather       = feather;
    e->fx_box.rate          = rate;
    e->fx_box.depth         = depth;
    e->fx_box.phase         = phase + s_draw.anim_phase;
    e->fx_box.rot           = rot;
    e->fx_box.abgr          = col;
    e->fx_box.variant       = variant;
    e->fx_box.cut_dx        = cut_dx;
    e->fx_box.cut_dy        = cut_dy;
    e->fx_box.swell         = swell;
    e->fx_box.border        = border;
    e->fx_box.shape         = shape;

    /* A pulse that names no curve breathes on the raised cosine it always has: a sawtooth would
       snap back to full every cycle, which is not what breathing is.  Resolved here, at the one
       site that knows the shape is pulsing at all.  A SWELL keeps the ambient as stated -- its
       sawtooth is the ripple restarting, which is what the linear default means there -- and a
       rate-0 swell is the value port, whose curve shapes the standing phase.  A shadow is not
       animated, so it takes no curve however the ambient is set -- the command hash must not
       move for a shape whose motion nothing reads. */

    bool animated = ( rate > 0.0f ) || ( swell != 0.0f );
    e->fx_box.curve       = !animated ? 0u
                          : ( swell == 0.0f && s_draw.anim_curve == GUI_CURVE_LINEAR )
                              ? (u32)GUI_CURVE_SINE : s_draw.anim_curve;
    e->fx_box.curve_param = animated ? s_draw.anim_curve_param : 0.0f;
    draw_cmd_seal();
}

void
draw_push_shadow( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, feather, GUI_FX_BOX_FILL, 0.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

/* x,y,w,h is the CASTER; (ox, oy) is how far the shadow falls from it.  The command carries the
   shadow's own rect, so the cut offset is the trip back to the caster -- the shape states where it
   is drawn and the offset states what it is drawn under. */
void
draw_push_skirt( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather,
                 f32 ox, f32 oy, u32 abgr )
{
    draw_fx_box_cmd( x + ox, y + oy, w, h, rounding, feather, GUI_FX_BOX_SKIRT,
                     0.0f, 0.0f, 0.0f, 0.0f, -ox, -oy, 0.0f, 0.0f, abgr );
}

/* The glow: the same surface with its outward falloff resolved EXPONENTIALLY (GUI_OP_GLOW) rather
   than through the feather's linear ramp.  Same geometry, same one quad, same batch -- the only
   difference is the curve, and it is the difference between a blurred edge and a lit one.
   `spread` is how far the light reaches, the draw_push_shadow vocabulary, so the feather carries
   twice it exactly as a shadow's does: the covering has to grow by the reach or the halo is cut
   off at the quad edge, and the tessellator derives the dropoff from the same number. */
void
draw_push_glow( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, feather, GUI_FX_BOX_GLOW, 0.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

/* The inner shadow: the same surface with its falloff turned inward (GUI_OP_INSET), painting
   from the boundary `depth` px in and nothing outside it.  A pressed well, a recessed field, the
   inner edge of a scroll area -- the shapes a drop shadow cannot make because they belong to the
   inside of their subject rather than to the ground under it. */
void
draw_push_inset( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 depth, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, depth, GUI_FX_BOX_INSET, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, abgr );
}

/* The hollow ring: the surface with its field bent into a border band of `t` px lying INSIDE the
   boundary (GUI_OP_BAND).  The ripple has always used this variant; what it did not have was a
   rect-taking verb, so an outline that swells, glows or breathes had to be a circle.  One quad,
   and the band is the shape's own field rather than a stroked perimeter -- which is what lets it
   trace a BAKED shape's silhouette exactly. */
void
draw_push_ring( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 t, u32 abgr )
{
    if ( t <= 0.0f )
        return;
    draw_fx_box_cmd( x, y, w, h, rounding, TESS_FX_AA, GUI_FX_BOX_RING, 0.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, t, abgr );
}

/* The plain fill: the surface with no op at all.  It exists for the BAKED shape, which has no
   other way to be drawn simply -- every other verb in this family carries an effect, and a shape
   wants to be paintable before it wants to glow. */
void
draw_push_shape( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, 0.0f, TESS_FX_AA, GUI_FX_BOX_FILL, 0.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

void
draw_push_pulse( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 rate, f32 depth, f32 phase,
                 u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, TESS_FX_AA, GUI_FX_BOX_FILL, rate, depth, phase, 0.0f,
                     0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

/* The swelling fill: the plain surface under GUI_OP_SWELL -- the boundary itself travels `amp`
   px past the authored rect (negative shrinks) as the clock's k runs 0..1, shaped by the ambient
   curve.  Geometry animating with byte-identical command bytes, the draw_push_pulse contract --
   where anim_ease, the CPU tween, re-tessellates every frame it moves.  At rate 0 the clock
   stands at `phase`: a static per-element size offset off one shared style -- the value port. */
void
draw_push_swell( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 rate, f32 amp, f32 phase,
                 u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, TESS_FX_AA, GUI_FX_BOX_FILL, rate, 0.0f, phase, 0.0f,
                     0.0f, 0.0f, amp, 0.0f, abgr );
}

/* The ripple: a hollow ring (GUI_OP_BAND) whose boundary swells `spread` px outward while a
   pulse of depth `fade` thins it away -- the sonar ping, expanding and dying on ONE clock, from
   one quad whose bytes never change while it runs.  The linear curve is the right default: its
   snap-back is the ripple restarting.  One shot: anim_once + draw_set_anim_phase, then stop
   drawing it, the phase-anchoring contract.  `r` is the ring's REST radius (it hugs its subject
   at k = 0), `thickness` the band width, `fade` 0..1 how gone it is at full stretch. */
void
draw_push_ripple( f32 cx, f32 cy, f32 r, f32 thickness, f32 spread, f32 rate, f32 phase,
                  f32 fade, u32 abgr )
{
    if ( r <= 0.0f || thickness <= 0.0f )
        return;
    draw_fx_box_cmd( cx - r, cy - r, r * 2.0f, r * 2.0f, r, TESS_FX_AA, GUI_FX_BOX_RING, rate, fade, phase,
                     0.0f, 0.0f, 0.0f, spread, thickness, abgr );
}

/* The rotated box: same surface, four corner positions turned about the box centre.  The default
   1 px AA is folded in here (a caller passing feather 0 wants a crisp edge, not a hard one) --
   the same bake draw_push_pulse does. */
void
draw_push_box_xf( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, f32 rot, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding,
                     ( feather > TESS_FX_AA ) ? feather : TESS_FX_AA,
                     GUI_FX_BOX_FILL, 0.0f, 0.0f, 0.0f, rot, 0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

/*==============================================================================================
    draw_push_round_rect_ex -- emit a filled box with four independent corner radii.

    The ambient rounding does NOT apply here and is not consulted: a caller reaching for this is
    naming every corner, and silently folding in a scope-level radius is how a tab ends up rounded
    on the two corners it wanted square.  The radii are clamped to the box at tessellation time
    (tess_fx_box_core), so an over-large one degenerates to a capsule rather than inverting.

    `feather` widens the falloff band exactly as draw_push_shadow's does: 0 gets the standard 1 px
    AA, wider makes the per-corner SOFT SHADOW -- the drop shadow under a tab or an asymmetric
    card, which draw_push_shadow (one radius) could not shape.  The quadrants agree at any feather
    (tess_fx_box_core's centre-line proof), so softness places no per-corner restriction.
==============================================================================================*/

void
draw_push_round_rect_ex( f32 x, f32 y, f32 w, f32 h,
                         f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather,
                         u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind, f32 grad_mid )
{
    /* The ramp midpoint, authored 0..1 (where the 50/50 blend lands along the ramp), mapped to
       the exponent the record carries: t^e crosses 0.5 at mid when e = ln 0.5 / ln mid.  0.5 and
       0 are the linear default and store 0, which is also what keeps two identical linear ramps
       authored either way deduping onto one record. */

    f32 mid_e = 0.0f;
    if ( grad_mid > 0.001f && grad_mid < 0.999f && grad_mid != 0.5f )
        mid_e = -0.69314718f / logf( grad_mid );

    /* Cull against the grown box: the falloff skirt is real geometry (feather/2 past the rect,
       plus the tessellator's pixel of slack) -- the draw_push_shadow rule. */

    f32 pad = ( feather > 0.0f ? feather * 0.5f : 0.0f ) + 1.0f;
    u32 col = draw_apply_alpha( abgr );
    u32 cb  = draw_apply_alpha( col_b );

    /* The transparent drop must see the WHOLE ramp: a gradient fading in from nothing has a
       transparent first endpoint and is still a real shape, the same nuance an outline-only
       text run relies on.  Visibility is therefore the stronger of the two alphas. */

    u32 vis = ( ( cb >> 24 ) > ( col >> 24 ) ) ? cb : col;

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_ROUND_RECT_EX, vis, x, y, w, h, pad );
    if ( !e )
        return;

    e->round_rect.x             = x;
    e->round_rect.y             = y;
    e->round_rect.w             = w;
    e->round_rect.h             = h;
    e->round_rect.rtl           = rtl;
    e->round_rect.rtr           = rtr;
    e->round_rect.rbr           = rbr;
    e->round_rect.rbl           = rbl;
    e->round_rect.feather       = feather;
    e->round_rect.corner_pow    = s_draw.corner_pow;
    e->round_rect.abgr          = col;
    e->round_rect.col_b         = cb;
    e->round_rect.grad_ang      = grad_ang;
    e->round_rect.grad_kind     = grad_kind;
    e->round_rect.grad_mid      = mid_e;

    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_arc / draw_push_pie -- emit a circular sector as one semantic command.

    Angles are radians in screen space (0 points +x, positive turns clockwise).  A reversed range
    and a sweep past a full turn are both normalized at tessellation, so a caller animating an
    angle never has to wrap it.

    The cull box is the whole circle rather than the sector's own bounds.  It is a conservative
    test against the scissor and nothing more -- computing the tight rotated extent to reject a few
    more off-screen arcs would cost every on-screen one two transcendentals it does not otherwise
    need at emit time.  The TESSELLATOR does compute the tight box, where it pays for itself in
    fragments rather than in a rejection that usually fails anyway.
==============================================================================================*/

static void
draw_sector_cmd( u8 type, f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                 f32 spin_rate, f32 spin_phase, u32 abgr )
{
    u32 col = draw_apply_alpha( abgr );
    f32 g   = r + thickness * 0.5f;   /* the tessellator's own AA pad rides draw_cmd_open's `pad` */

    gui_cmd_ext_t* e = draw_cmd_open( type, col, cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !e )
        return;

    /* A static sector reads neither its phase nor a curve (tess_fx_arc applies both only under
       GUI_OP_SPIN), so neither may reach its bytes: the command hash must not move when an
       ambient nothing reads changes -- the same rule draw_fx_box_cmd applies to a shadow. */
    bool spinning = ( spin_rate != 0.0f );

    e->arc.cx           = cx;
    e->arc.cy           = cy;
    e->arc.r            = r;
    e->arc.thickness    = thickness;
    e->arc.a0           = a0;
    e->arc.a1           = a1;
    e->arc.spin_rate    = spin_rate;
    e->arc.spin_phase   = spinning ? spin_phase + s_draw.anim_phase : 0.0f;
    e->arc.abgr         = col;
    e->arc.curve        = spinning ? s_draw.anim_curve : 0u;
    e->arc.curve_param  = spinning ? s_draw.anim_curve_param : 0.0f;

    draw_cmd_seal();
}

void
draw_push_arc( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1, u32 abgr )
{
    draw_sector_cmd( GUI_CMD_ARC, cx, cy, r, thickness, a0, a1, 0.0f, 0.0f, abgr );
}

/* The arc under GUI_OP_SPIN: its whole frame rotates at `rate` turns/sec on the shader clock, so
   the command's bytes are identical every frame it runs -- the spinner that re-tessellates
   nothing.  `phase` is the starting angle in turns.  The caller still presents frames
   (gui()->request_redraw(), the pulse contract). */
void
draw_push_arc_spin( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                    f32 rate, f32 phase, u32 abgr )
{
    draw_sector_cmd( GUI_CMD_ARC, cx, cy, r, thickness, a0, a1, rate, phase, abgr );
}

void
draw_push_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 abgr )
{
    draw_sector_cmd( GUI_CMD_PIE, cx, cy, r, 0.0f, a0, a1, 0.0f, 0.0f, abgr );
}

/*==============================================================================================
    draw_push_arc_dashed / draw_push_arc_gradient -- the self-sampled sector variants.

    Both are the plain arc's geometry plus one op on the sector's boundary coordinate
    (GUI_OP_DASH / GUI_OP_GRAD_ALONG, gui.h).  Emit's share of the work:

    DASH quantizes the caller's pixel vocabulary (dash/gap arc-length px at radius r, the
    draw_dashed_line terms) into an angular period that divides the sweep a WHOLE number of times.
    Snapping here rather than in the fragment is what keeps a closed dashed ring from showing a
    seam where the pattern meets itself -- and it costs one round() per push, not per pixel.

    GRADIENT folds the ambient alpha into BOTH ends; visibility is the OR of the folded colours,
    the draw_push_rect_gradient rule.
==============================================================================================*/

#define DRAW_TAU  6.28318530717959f

void
draw_push_arc_dashed( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                      f32 dash, f32 gap, u32 abgr )
{
    if ( r <= 0.0f || dash <= 0.0f )
        return;

    /* Angular period from the pixel vocabulary, then snapped so N whole cycles fit the sweep. */
    f32 sweep = a1 - a0;
    if ( sweep < 0.0f ) sweep = -sweep;
    if ( sweep > DRAW_TAU ) sweep = DRAW_TAU;
    f32 period = ( dash + ( gap > 0.0f ? gap : dash ) ) / r;
    f32 n      = floorf( sweep / period + 0.5f );
    if ( n < 1.0f ) n = 1.0f;
    period = sweep / n;

    u32 col = draw_apply_alpha( abgr );
    f32 g   = r + thickness * 0.5f;

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_ARC_DASH, col, cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !e )
        return;
    e->arc_dash.cx        = cx;
    e->arc_dash.cy        = cy;
    e->arc_dash.r         = r;
    e->arc_dash.thickness = thickness;
    e->arc_dash.a0        = a0;
    e->arc_dash.a1        = a1;
    e->arc_dash.period    = period;
    e->arc_dash.duty      = dash / ( dash + ( gap > 0.0f ? gap : dash ) );
    e->arc_dash.abgr      = col;
    draw_cmd_seal();
}

void
draw_push_arc_gradient( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                        u32 col_a, u32 col_b )
{
    if ( r <= 0.0f )
        return;

    /* Normalize a reversed range HERE, not at tessellation: the tessellator's swap is invisible
       for a symmetric arc but this one is not -- swapping the endpoints without swapping the
       colours would silently flip the gradient. */
    if ( a1 < a0 )
    {
        f32 t = a0; a0 = a1; a1 = t;
        u32 u = col_a; col_a = col_b; col_b = u;
    }

    /* Visible if EITHER end is -- the OR'd alpha is the visibility word draw_cmd_open tests. */
    u32 ca = draw_apply_alpha( col_a );
    u32 cb = draw_apply_alpha( col_b );
    f32 g  = r + thickness * 0.5f;

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_ARC_GRAD, ca | cb,
                                          cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !e )
        return;
    e->arc_grad.cx        = cx;
    e->arc_grad.cy        = cy;
    e->arc_grad.r         = r;
    e->arc_grad.thickness = thickness;
    e->arc_grad.a0        = a0;
    e->arc_grad.a1        = a1;
    e->arc_grad.col_a     = ca;
    e->arc_grad.col_b     = cb;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_checker / draw_push_grid -- the framebuffer-tiling pattern quads.

    Each is ONE quad whose fragment tiles the pattern in framebuffer pixels (GUI_OP_CHECKER /
    GUI_OP_GRID, gui.h); the CPU's share -- quantizing the cell pitch and deriving the anchor
    phase against it -- runs at tessellation, where the box has been snapped.  Emit just gates
    and stores the semantic fields.
==============================================================================================*/

void
draw_push_checker( f32 x, f32 y, f32 w, f32 h, f32 cell, u32 col_a, u32 col_b )
{
    if ( cell < 1.0f )
        cell = 1.0f;

    /* Visible if EITHER colour is -- the OR'd alpha, the two-colour rule (draw_push_rect_gradient). */
    u32 ca = draw_apply_alpha( col_a );
    u32 cb = draw_apply_alpha( col_b );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_CHECKER, ca | cb, x, y, w, h, 0.0f );
    if ( !e )
        return;
    e->checker.x     = x;
    e->checker.y     = y;
    e->checker.w     = w;
    e->checker.h     = h;
    e->checker.cell  = cell;
    e->checker.col_a = ca;
    e->checker.col_b = cb;
    e->checker.rounding   = s_draw.rounding;
    e->checker.corner_pow = ( s_draw.rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    draw_cmd_seal();
}

void
draw_push_grid( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 angle, bool stripes,
                f32 cell, f32 thickness, u32 abgr )
{
    /* A lattice denser than its own line width is a fill; keep the parameters meaning what they
       say rather than letting the fragment resolve a moire. */
    if ( thickness < 1.0f ) thickness = 1.0f;
    if ( cell < 2.0f ) cell = 2.0f;
    if ( cell < thickness ) cell = thickness;

    u32 col = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_GRID, col, x, y, w, h, 0.0f );
    if ( !e )
        return;
    e->grid.x         = x;
    e->grid.y         = y;
    e->grid.w         = w;
    e->grid.h         = h;
    e->grid.cell      = cell;
    e->grid.thickness = thickness;
    e->grid.ox        = ox;
    e->grid.oy        = oy;
    e->grid.angle     = angle;
    e->grid.stripes   = stripes ? 1u : 0u;
    e->grid.abgr      = col;
    e->grid.rounding   = s_draw.rounding;
    e->grid.corner_pow = ( s_draw.rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_ngon -- a regular polygon as one GUI_FX_NGON quad, filled or stroked.

    The polyline fan this replaces sampled up to 64 perimeter points; the field is exact at any
    size and the corner can round.  `rounding` shrinks the polygon and inflates the field back
    out, so the stated circumradius is the size drawn.  The border-align ambient applies to the
    stroked form exactly as it does to the rect outline -- same inflation, same reasoning.

    `star` in (0..1) pulls each edge midpoint in to star * r, drawing a `sides`-pointed star
    from the same field; 0 keeps the regular polygon.  The ratio is capped just under the
    apothem, past which the "star" would bow outside the polygon it is cut from.
==============================================================================================*/

void
draw_push_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, f32 rounding,
                f32 thickness, f32 star, u32 abgr )
{
    if ( r <= 0.0f )
        return;
    if ( sides < 3u )  sides = 3u;
    if ( sides > 64u ) sides = 64u;
    if ( star < 0.0f ) star = 0.0f;

    if ( thickness > 0.0f )
    {
        f32 ba = s_draw.border_align * thickness;
        r += ba;
        if ( rounding > 0.0f ) rounding += ba;
    }
    if ( rounding > r * 0.5f ) rounding = r * 0.5f;   /* the field needs a real core to inflate from */

    u32 col = draw_apply_alpha( abgr );
    f32 g   = r + 1.0f;

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_NGON, col, cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !e )
        return;
    e->ngon.cx        = cx;
    e->ngon.cy        = cy;
    e->ngon.r         = r;
    e->ngon.rounding  = rounding;
    e->ngon.rot       = rot;
    e->ngon.thickness = thickness;
    e->ngon.star      = star;
    e->ngon.sides     = sides;
    e->ngon.abgr      = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_box_dashed -- a rounded-box outline cut by a perimeter dash (the marching ants).

    dash/gap are arc-length px, the draw_dashed_line vocabulary; the tessellator snaps the period
    so whole cycles fit the perimeter and the pattern meets itself.  `rate` scrolls it in px/sec
    on the shader clock (0 = static), so the ants' command bytes never change while they march.
==============================================================================================*/

void
draw_push_box_dashed( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 t,
                      f32 dash, f32 gap, f32 rate, f32 phase, u32 abgr )
{
    if ( dash <= 0.0f || t <= 0.0f )
        return;

    /* Border alignment: the same push-time inflation the plain outline runs. */
    f32 ba = s_draw.border_align * t;
    if ( ba > 0.0f )
    {
        x -= ba;  y -= ba;  w += ba * 2.0f;  h += ba * 2.0f;
        if ( rounding > 0.0f ) rounding += ba;
    }

    u32 col = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_BOX_DASH, col, x, y, w, h, 1.0f );
    if ( !e )
        return;
    e->box_dash.x        = x;
    e->box_dash.y        = y;
    e->box_dash.w        = w;
    e->box_dash.h        = h;
    e->box_dash.rounding = rounding;
    e->box_dash.t        = t;
    e->box_dash.dash     = dash;
    e->box_dash.gap      = gap;
    e->box_dash.rate     = rate;
    e->box_dash.phase    = phase;
    e->box_dash.anim_phase = s_draw.anim_phase;
    e->box_dash.abgr     = col;
    e->box_dash.curve       = s_draw.anim_curve;
    e->box_dash.curve_param = s_draw.anim_curve_param;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_box_trace -- ONE arc travelling the rounded-box border.

    The dashed border above with its period set to the WHOLE perimeter, so the tessellator's
    snap can only fit a single cycle and `frac` of the border is lit.

    The perimeter is measured HERE rather than by the caller because it can only be known after
    the border alignment below has moved the boundary it is measured along -- a caller stating a
    period against the un-aligned box would hand the snap a ratio off by the alignment, and two
    arcs where one was asked for is a visibly different shape.

    `laps` scrolls the arc on the shader clock in revolutions/sec: the command's bytes do not
    change while it runs, so an indeterminate tracer re-tessellates nothing (present frames with
    request_redraw -- the draw_pulse contract).  `at` places it statically instead, 0..1 around
    the border from the top-left; that value IS in the command's bytes, so a determinate trace
    re-tessellates its window when it moves, exactly as a progress bar's fill width does.
==============================================================================================*/

void
draw_push_box_trace( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 t,
                     f32 frac, f32 laps, f32 at, u32 abgr )
{
    if ( t <= 0.0f || w <= 0.0f || h <= 0.0f || frac <= 0.0f )
        return;
    if ( frac > 1.0f )
        frac = 1.0f;

    /* Border alignment: the same push-time inflation the plain outline runs, and it has to happen
       before the measurement below. */
    f32 ba = s_draw.border_align * t;
    if ( ba > 0.0f )
    {
        x -= ba;  y -= ba;  w += ba * 2.0f;  h += ba * 2.0f;
        if ( rounding > 0.0f ) rounding += ba;
    }

    /* The perimeter, against the SAME clamped radius the tessellator will state: four edges
       shortened by the corners they meet, plus the four quarter-arcs (gui_build_tess_dispatch.c,
       GUI_CMD_BOX_DASH). */
    f32 hw  = w * 0.5f, hh = h * 0.5f;
    f32 lim = ( hw < hh ) ? hw : hh;
    f32 r   = rounding;
    if ( r > lim )  r = lim;
    if ( r < 0.0f ) r = 0.0f;

    f32 len = 4.0f * ( hw + hh ) - 8.0f * r + DRAW_TAU * r;
    if ( len <= 0.0f )
        return;

    u32 col = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_BOX_DASH, col, x, y, w, h, 1.0f );
    if ( !e )
        return;
    e->box_dash.x        = x;
    e->box_dash.y        = y;
    e->box_dash.w        = w;
    e->box_dash.h        = h;
    e->box_dash.rounding = rounding;
    e->box_dash.t        = t;
    /* One cycle spans the whole border, so the snap resolves to exactly one arc and the duty --
       dash / (dash + gap) -- is `frac` however the measurement rounded. */
    e->box_dash.dash     = frac * len;
    e->box_dash.gap      = ( 1.0f - frac ) * len;
    e->box_dash.rate     = laps * len;   /* px/sec, which over a one-period border is laps/sec */
    e->box_dash.phase    = at   * len;
    e->box_dash.anim_phase  = s_draw.anim_phase;
    e->box_dash.abgr        = col;
    e->box_dash.curve       = s_draw.anim_curve;
    e->box_dash.curve_param = s_draw.anim_curve_param;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_box_cut -- a rounded box minus a SECOND rounded box (GUI_OP_CUT_SHAPE).

    The cut's centre arrives ABSOLUTE and leaves as an offset from the shape's centre, which is
    the form the record stores (the cut vector).  The culled box is the fill's own -- the cut
    only ever removes ink, so the fill's bounds still bound the shape.
==============================================================================================*/

void
draw_push_box_cut( f32 x, f32 y, f32 w, f32 h, f32 rounding,
                   f32 cut_cx, f32 cut_cy, f32 cut_w, f32 cut_h, f32 cut_r, f32 soft,
                   u32 abgr )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    u32 col = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_BOX_CUT, col, x, y, w, h, 1.0f );
    if ( !e )
        return;
    e->box_cut.x        = x;
    e->box_cut.y        = y;
    e->box_cut.w        = w;
    e->box_cut.h        = h;
    e->box_cut.rounding = rounding;
    e->box_cut.cut_dx   = cut_cx - ( x + w * 0.5f );
    e->box_cut.cut_dy   = cut_cy - ( y + h * 0.5f );
    e->box_cut.cut_w    = cut_w;
    e->box_cut.cut_h    = cut_h;
    e->box_cut.cut_r    = cut_r;
    e->box_cut.cut_aa   = soft;
    e->box_cut.abgr     = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_repeat -- a lattice of one rounded cell, from ONE quad.

    nx by ny copies, `pitch` apart centre-to-centre, centred on (cx, cy).  The fragment folds its
    coordinate into a cell (GUI_OP_REPEAT), so a 3x3 grip and a 40-tick ruler cost the same one
    quad and the same one prim record -- which is the point: a lattice was affordable before only
    while the count stayed small, and the counts a ruler or a segmented bar actually wants did not.

    The SET's box is derived rather than taken, here and in the tessellator, because the fragment
    recovers the copy count from it (tess_repeat_box owns that contract).  So this culls against a
    box it computes for itself.
==============================================================================================*/

void
draw_push_repeat( f32 cx, f32 cy, u32 nx, u32 ny, f32 pitch_x, f32 pitch_y,
                  f32 cell_w, f32 cell_h, f32 rounding, u32 abgr, u32 col_b, f32 fill )
{
    if ( nx == 0u || ny == 0u || cell_w <= 0.0f || cell_h <= 0.0f )
        return;

    /* The same flooring the tessellator applies, so the box culled against is the box drawn. */
    f32 px = ( pitch_x > cell_w ) ? pitch_x : cell_w + 1.0f;
    f32 py = ( pitch_y > cell_h ) ? pitch_y : cell_h + 1.0f;
    f32 hx = (f32)( nx - 1u ) * 0.5f * px + cell_w * 0.5f;
    f32 hy = (f32)( ny - 1u ) * 0.5f * py + cell_h * 0.5f;

    /* Both ends fold the ambient alpha, and "no ramp" (col_b == abgr) survives the fold because
       the two fold identically.  Visible if EITHER end is, the gradient rule. */
    u32 col = draw_apply_alpha( abgr );
    u32 cb  = draw_apply_alpha( col_b );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_REPEAT, col | cb,
                                          cx - hx, cy - hy, hx * 2.0f, hy * 2.0f, 1.0f );
    if ( !e )
        return;
    e->repeat.cx       = cx;
    e->repeat.cy       = cy;
    e->repeat.pitch_x  = pitch_x;
    e->repeat.pitch_y  = pitch_y;
    e->repeat.cell_w   = cell_w;
    e->repeat.cell_h   = cell_h;
    e->repeat.rounding = rounding;
    e->repeat.nx       = nx;
    e->repeat.ny       = ny;
    e->repeat.abgr     = col;
    e->repeat.col_b    = cb;
    e->repeat.fill     = fill;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_repeat_polar -- a RING of one rounded cell, from ONE quad.

    `n` copies on a circle of radius `orbit` about (cx, cy).  The angular twin of the lattice
    above, and at a non-zero `rate` (revolutions/sec) the ring turns on the shader clock in the
    FRAGMENT -- so these bytes are identical every frame and a spinner re-tessellates nothing,
    exactly like draw_push_pulse.  The caller still owes one request_redraw per frame while it
    shows: the clock advancing is not what schedules a frame (GUI_FX_TIME_WRAP).
==============================================================================================*/

void
draw_push_repeat_polar( f32 cx, f32 cy, u32 n, f32 orbit, f32 cell_w, f32 cell_h,
                        f32 rounding, f32 rate, f32 phase, u32 abgr, u32 col_b )
{
    if ( n == 0u || orbit <= 0.0f || cell_w <= 0.0f || cell_h <= 0.0f )
        return;

    /* The same bound the tessellator derives, so the box culled against is the box drawn. */
    f32 hx = orbit + cell_w * 0.5f;
    f32 hy = orbit + cell_h * 0.5f;

    /* Both colours fold the ambient alpha, and "no ramp" (col_b == abgr) survives the fold
       because the two fold identically.  Visible if EITHER end is, the gradient rule. */
    u32 col = draw_apply_alpha( abgr );
    u32 cb  = draw_apply_alpha( col_b );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_REPEAT_POLAR, col | cb,
                                          cx - hx, cy - hy, hx * 2.0f, hy * 2.0f, 1.0f );
    if ( !e )
        return;
    e->repeat_polar.cx       = cx;
    e->repeat_polar.cy       = cy;
    e->repeat_polar.orbit    = orbit;
    e->repeat_polar.cell_w   = cell_w;
    e->repeat_polar.cell_h   = cell_h;
    e->repeat_polar.rounding = rounding;
    e->repeat_polar.rate     = rate;
    e->repeat_polar.phase    = phase + s_draw.anim_phase;
    e->repeat_polar.n        = n;
    e->repeat_polar.abgr     = col;
    e->repeat_polar.col_b    = cb;
    /* A static ring takes no curve however the ambient is set: the command hash must not move for
       a shape whose motion nothing reads.  The same rule draw_fx_box_cmd applies to a shadow. */
    e->repeat_polar.curve       = ( rate > 0.0f ) ? s_draw.anim_curve : 0u;
    e->repeat_polar.curve_param = ( rate > 0.0f ) ? s_draw.anim_curve_param : 0.0f;
    draw_cmd_seal();
}

// clang-format on
/*============================================================================================*/
