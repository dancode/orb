/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess_dispatch.c -- polylines + the dispatcher.

    Part 7 of 7 of the CPU-side quad-record builder (see gui_build_tess_state.c for the family
    overview).  Holds the polyline stroker (tess_stroke_poly_aa, tess_fx_segment -- the capsule
    that backs both a single GUI_CMD_LINE and every segment of a path) and tess_dispatch itself:
    the per-command-type switch that drives every tessellator in this file family from the
    frame's semantic command list.

    Included last in the family, right after gui_build_tess_text.c (needs every tess_* emitter
    the switch below calls) and right before gui_build_volatile.c, whose volatile_range_close
    tess_dispatch calls to close out a tagged command range.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    tess_stroke_poly_aa -- the polyline as a CAPSULE CHAIN: one SEG quad per segment, endpoints
    offset along the miter normals so alignment matches the old ribbon stroker.  Every quad
    resolves to ONE shared style (the direction rides the quad, tess_fx_segment), so a long path
    costs segments, not records.  Joins are the round caps overlapping, which composites darker
    on a translucent stroke -- the accepted trade for retiring the miter ribbon.
    abgr is pre-baked (alpha folded in at emit time).  v2 / seg_normal / stroke_center_offset
    are defined in gui_emit_path.c (included before this file in the unity build).
==============================================================================================*/

static void tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 border,
                             u32 abgr );   /* the quad backend's polyline expansion, defined below */

static void
tess_stroke_poly_aa( const gui_vec2_t* pts, u32 n, f32 thickness, f32 center_off,
                     bool closed, u32 abgr )
{
    if ( n < 2 )
        return;
    if ( n > GUI_MAX_PATH_PTS )
         n = GUI_MAX_PATH_PTS;

    /* Sub-pixel coverage: hold a 1px footprint, fade peak alpha by the requested thickness.
       Done here rather than left to tess_fx_segment's own clamp so the fold below is already
       final -- the segment's re-fold is then a no-op and every segment shares one colour. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )
    {
        a_scale   = thickness < 0.0f ? 0.0f : thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    u32 seg = closed ? n : n - 1;

    /* Per-point miter normal; static avoids an 8K+ stack frame. Single-threaded. */
    static gui_vec2_t nrm[ GUI_MAX_PATH_PTS ];
    for ( u32 i = 0; i < n; ++i )
    {
        gui_vec2_t n0, n1;
        if ( closed )
        {
            n0 = seg_normal( pts[ ( i + n - 1 ) % n ], pts[ i ] );
            n1 = seg_normal( pts[ i ], pts[ ( i + 1 ) % n ] );
        }
        else
        {
            n0 = ( i > 0 )     ? seg_normal( pts[ i - 1 ], pts[ i ] ) : v2( 0.0f, 0.0f );
            n1 = ( i < n - 1 ) ? seg_normal( pts[ i ], pts[ i + 1 ] ) : v2( 0.0f, 0.0f );
        }

        if ( !closed && i == 0 )          nrm[ i ] = n1;
        else if ( !closed && i == n - 1 ) nrm[ i ] = n0;
        else
        {
            gui_vec2_t dm = v2( ( n0.x + n1.x ) * 0.5f, ( n0.y + n1.y ) * 0.5f );
            f32 d2 = dm.x * dm.x + dm.y * dm.y;
            if ( d2 > 1e-6f )
            {
                f32 inv = 1.0f / d2;
                if ( inv > 100.0f ) inv = 100.0f;   /* miter limit */
                dm.x *= inv; dm.y *= inv;
                nrm[ i ] = dm;
            }
            else { nrm[ i ] = n1; }
        }
    }

    for ( u32 s2 = 0; s2 < seg; ++s2 )
    {
        u32 j = ( s2 + 1u ) % n;
        tess_fx_segment( pts[ s2 ].x + nrm[ s2 ].x * center_off,
                         pts[ s2 ].y + nrm[ s2 ].y * center_off,
                         pts[ j ].x + nrm[ j ].x * center_off,
                         pts[ j ].y + nrm[ j ].y * center_off,
                         thickness, 0.0f, col );
    }
}

/*==============================================================================================
    tess_fx_segment -- one line segment as a CAPSULE distance field: the distance from a point
    to a segment, minus the half-thickness.  One quad under the CAPSULE rule, an edge that is
    correct at any angle, and round caps that cost nothing because they ARE the field.

    Round caps extend half a thickness past each endpoint.  On a polyline (the capsule chain,
    tess_stroke_poly_aa) they are also the JOINS: neighbouring capsules overlap there, which
    composites darker on a translucent stroke -- the accepted cost of per-segment records.

    Axis-aligned single lines never come here: gui_draw_line routes them through a grid-snapped
    rect at EMIT (stroke_axis_aligned_rect, gui_emit_path.c), which is crisper than any field
    since a horizontal edge has nothing to antialias.
==============================================================================================*/

static void
tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 border, u32 abgr )
{
    if ( thickness <= 0.0f )
        return;

    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;

    /* Sub-pixel coverage, matched to the ribbon stroker rather than left to the field: hold a 1 px
       footprint and fade peak alpha.  The field would happily render a 0.4 px capsule, but it would
       weigh a hairline differently than every other line in the library, and consistency across the
       two paths is worth more here than the extra correctness. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )                 /* thickness > 0 by the guard above */
    {
        a_scale   = thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    f32 inv = 1.0f / len;
    f32 ux  = dx * inv, uy = dy * inv;      /* unit vector along the segment  */
    f32 r   = thickness * 0.5f;             /* the capsule radius             */
    f32 hl  = len * 0.5f;                   /* half-length: what q.x subtracts */
    f32 mx  = ( x0 + x1 ) * 0.5f, my = ( y0 + y1 ) * 0.5f;

    /* The style states the capsule's radius in the corner-radius lane.  The DIRECTION is the
       quad's own turn, like every other shape's -- which is exactly why every segment of every
       stroke at one thickness shares ONE style: the turn was never in the style to forfeit.  The
       half-length rides the quad (rect.z). */
    s_tess.cur_prim.field   = (u32)GUI_FX_SEG;
    s_tess.cur_prim.r_tl    = r;
    s_tess.cur_prim.feather = TESS_FX_AA;

    /* A HOLLOW capsule is the same field under GUI_OP_BAND -- the op that makes a rounded outline
       out of a filled box, reaching this shape because an op modifies whatever field arrived.  A
       border at or past the radius has no interior left to remove, so it stays filled rather than
       inverting into one. */
    if ( border > 0.0f && border < r )
    {
        s_tess.cur_ops         |= GUI_OP_BAND;
        s_tess.cur_prim.border  = border;
    }

    s_tess.cur_ops  |= GUI_OP_SELF;
    s_tess.cur_rot_c = ux;
    s_tess.cur_rot_s = uy;
    tess_quad_push( mx, my, hl, r, GUI_QUAD_RULE_CAPSULE, 0, 0,
                    res_atlas_idx(), col, GUI_GLYPH_ID_NONE );
}

/* Volatile-widget seam (render/pipeline/gui_build_volatile.c, included right after this file in
   the gui_render.c unity build).  tess_dispatch calls volatile_range_close once a tagged command
   RANGE's quads / style records / GPU commands are fully written; it records the block's slot-relative
   position, reserves padded headroom past the live geometry (advancing this file's write heads),
   and stamps the slot tessellation generation.  s_volatile_patching is declared up with s_tess
   (tess_quad_push reads it to keep a patch out of the glyph counters) and set by volatile_patch around its scratch re-tessellation so the range tracking below
   stays inert during a patch -- a patch must never look like a fresh capture. */
static void volatile_range_close( gui_id_t id, u32 vb_open, u32 pb_open, u32 cmd_open );

/* Tessellate one frame's semantic command list into s_tess geometry.

   `order` is a permutation of [0,count): the window's visible commands in emission order (built
   by cache_tess_window; clip-empty commands are already dropped).  Nothing about clips shapes it
   any more -- the clip rides the vertex (the clip band) and cannot cut a draw call.
   `win` is the window being tessellated (informational; volatile rows already know their window
   from emit-time stamping).

   The FONT is activated per TEXT COMMAND, from the command's own font id, and only by the two cases
   that read glyphs.  It used to arrive as a `fonts[]` array parallel to `order` -- one entry per
   ordered command, reconstructing a per-segment property after the clip sort had torn the segments
   apart -- and it used to be switched at the top of this loop for every command, text or not.
   Neither was needed: a fill, a line and a sprite never call font_glyph, and the font is per-command
   data now (gui.h).  Activating it changes which atlas the glyph lookups resolve from and nothing
   else; it does NOT split the GPU batch, since it only alters the texture tess_set_tex stamps onto
   the following quads' style records.  A bitmap label, an SDF heading and the fill behind them still go out as one
   draw call.  The active font is saved and restored so the BUILD phase leaves the global font state
   (used by the next frame's layout) untouched. */
static void
tess_dispatch( const gui_cmd_t* cmds, const u16* order, u32 count, gui_id_t win )
{
    u32 saved_font = font_active_id();
    u32 cur_font   = saved_font;

    /* Volatile-widget range tracking: cmd_volatile_id tags a contiguous RANGE of commands (not
       just one), so bracket [vb_open, ...) / [ib_open, ...) / [cmd_open, ...) while the tag stays
       the same and hand the finished range to volatile_range_close when it changes (or at the
       end).  force_new_cmd is raised when a range OPENS so the block's geometry lands in its own
       fresh GPU command(s), never merged with a neighbour's -- the block's elem_counts must stay
       independently rewritable by a later patch.  Tracking is inert during a patch's own scratch
       re-tessellation (s_volatile_patching). */
    gui_id_t open_vid = GUI_ID_NONE;
    u32      vb_open = 0, pb_open = 0, cmd_open = 0;
    (void)win;

    /* Attribute the records this pass commits to their window, and to this tessellation pass --
       the census counts a record's PASSES to show the cross-slot spread that slot-scoped dedup
       can never collapse. */
    PRIM_CENSUS_WINDOW( win, s_tess.slot_tess_gen );

    for ( u32 oi = 0; oi < count; ++oi )
    {
        u32              ci = order[ oi ];
        const gui_cmd_t* c  = &cmds[ ci ];

        gui_id_t vid = s_volatile_patching ? GUI_ID_NONE : s_draw.cmd_volatile_id[ ci ];
        if ( vid != open_vid )
        {
            if ( open_vid != GUI_ID_NONE )
                volatile_range_close( open_vid, vb_open, pb_open, cmd_open );
            open_vid = vid;

            /* The dedup floor rises at BOTH sides of a volatile boundary.  Entering: the block's
               first primitive must append a record INSIDE its own range instead of reusing the
               window's preceding one -- a reused record would sit outside the reservation the
               patch is allowed to rewrite, and the patch (which always starts cold) would then
               disagree with the capture about how many it needs.  Leaving: the block's records
               ARE that rewritable reservation, so a later command deduping onto one would be
               corrupted by the next patch. */
            s_tess.prim_dedup_floor = s_tess.prim_count;
            tess_fx_page_reset();

            if ( vid != GUI_ID_NONE )
            {
                s_tess.force_new_cmd = true;   /* block owns its GPU commands from the first primitive */
                vb_open  = s_tess.quad_count;
                pb_open  = s_tess.prim_count;
                cmd_open = s_tess.cmd_count;
            }
        }

        s_tess.cur_clip       = s_draw.clip_table[ c->clip_idx ];
        s_tess.cur_clip_local = tess_clip_local( c->clip_idx );
        s_tess.cur_vp         = c->vp;

        /* The op word is ambient over ONE command and cleared here, so a case that sets it
           cannot leak the effect onto the next primitive.  That containment is the whole reason
           it can be ambient at all -- it lets an outline reach every glyph of a run without
           threading a parameter through tess_rect_filled, which every fill in the library
           shares. */
        s_tess.cur_ops        = 0u;
        s_tess.cur_corner_pow = 0.0f;
        s_tess.cur_col_border = 0u;
        s_tess.cur_rot_c      = 1.0f;
        s_tess.cur_rot_s      = 0.0f;
        s_tess.cur_phase      = 0.0f;
        s_tess.cur_swell      = 0.0f;

        /* The record is cleared WHOLE, and it matters for two reasons: a leftover rect or radius
           does not merely paint wrong, it defeats the memo -- a run of flat fills carrying stale
           geometry would take one record each. */
        s_tess.cur_prim = ( gui_prim_t ){ 0 };

        switch ( c->type )
        {
            /* A square rect keeps the one-quad fast path: it is pixel-aligned by construction, so
               there is no edge for an SDF to resolve and nothing to gain.  Rounding is what turns
               it into a surface -- and routing the TEXTURED case through as well is what finally
               lets a rounded quad carry an image, which the arc fan never could. */
            case GUI_CMD_RECT_FILLED:
                if ( c->rect.rounding > 0.0f )
                {
                    s_tess.cur_corner_pow = c->rect.corner_pow;
                    tess_fx_box( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                 c->rect.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                                 c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                 c->rect.tex_idx, c->rect.abgr, NULL );
                }
                else
                    tess_rect_filled( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                      c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                      c->rect.tex_idx, c->rect.abgr );
                break;

            /* The band measures from the OUTER boundary inward, matching the square path's
               INSIDE band (and the closed AA stroke this replaced). */
            case GUI_CMD_RECT_OUTLINE:
                if ( c->rect_outline.rounding > 0.0f )
                {
                    s_tess.cur_ops       |= GUI_OP_BAND;
                    s_tess.cur_corner_pow = c->rect_outline.corner_pow;
                    tess_fx_box( c->rect_outline.x, c->rect_outline.y,
                                 c->rect_outline.w, c->rect_outline.h,
                                 c->rect_outline.rounding, TESS_FX_AA, c->rect_outline.t,
                                 0.0f, 0.0f, 0.0f,
                                 0, 0, 1, 1, 0, c->rect_outline.abgr, NULL );
                }
                else
                    tess_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                       c->rect_outline.w, c->rect_outline.h,
                                       c->rect_outline.t, c->rect_outline.abgr );
                break;

            /* Body + border in one surface.  A square frame runs the field with feather 0 -- a
               hard cut on the snapped boundary, matching the crisp edges the fill + four-rail
               pair drew -- and a rounded one takes the standard AA band. */
            case GUI_CMD_FRAME:
                s_tess.cur_ops       |= GUI_OP_FRAME;
                s_tess.cur_corner_pow = c->frame.corner_pow;
                s_tess.cur_col_border = c->frame.col_border;   /* rides the quad, not the style */
                tess_fx_box( c->frame.x, c->frame.y, c->frame.w, c->frame.h,
                             c->frame.rounding,
                             ( c->frame.rounding > 0.0f ) ? TESS_FX_AA : 0.0f,
                             c->frame.t, 0.0f, 0.0f, 0.0f,
                             0, 0, 1, 1, 0, c->frame.abgr, NULL );
                break;

            /* The parameterized surface: a shadow is the wide feather (what used to be six
               stacked rects pretending to be a gaussian is the exact same falloff the corners
               use, only spread out), a pulse the shader-clock word -- geometrically a plain
               rounded fill whose record is correct for every frame it runs, so the retained
               slot never invalidates and the breathing costs no re-tessellation. */
            case GUI_CMD_FX_BOX:
                /* All of these are the one GUI_FX_BOX mode; the variant, the rate and the swell
                   pick which ops ride the tex word.  They are INDEPENDENT flags rather than a
                   choice of one, which is what lets a cut or inset surface breathe -- as a mode
                   number the pulse had to displace whichever shape it was applied to.  Set before
                   the tessellator runs because the interior hole is sized from them (see
                   `reach`). */
                if ( c->fx_box.variant == GUI_FX_BOX_SKIRT )   s_tess.cur_ops |= GUI_OP_CUT;
                if ( c->fx_box.variant == GUI_FX_BOX_INSET )   s_tess.cur_ops |= GUI_OP_INSET;
                if ( c->fx_box.variant == GUI_FX_BOX_GLOW )    s_tess.cur_ops |= GUI_OP_GLOW;
                if ( c->fx_box.variant == GUI_FX_BOX_RING )    s_tess.cur_ops |= GUI_OP_BAND;
                if ( c->fx_box.rate    > 0.0f )  s_tess.cur_ops |= GUI_OP_PULSE;
                if ( c->fx_box.swell  != 0.0f )  s_tess.cur_ops |= GUI_OP_SWELL;
                {
                    /* The cut boundary, for the DIRECTIONAL cast: the command states where the
                       shadow is drawn, and this says where the caster it belongs to sits relative
                       to it.  Zero for every other variant, and the aux is read only under the op
                       that owns it. */
                    tess_fx_aux_t aux = { 0 };
                    aux.cut_dx     = c->fx_box.cut_dx;
                    aux.cut_dy     = c->fx_box.cut_dy;
                    aux.anim_phase = c->fx_box.phase;
                    aux.anim_curve = c->fx_box.curve;
                    aux.anim_param = c->fx_box.curve_param;
                    aux.swell_amp  = c->fx_box.swell;
                    s_tess.cur_corner_pow = c->fx_box.corner_pow;
                    tess_fx_box( c->fx_box.x, c->fx_box.y, c->fx_box.w, c->fx_box.h,
                                 c->fx_box.rounding, c->fx_box.feather, c->fx_box.border,
                                 c->fx_box.rate, c->fx_box.depth, c->fx_box.rot,
                                 0, 0, 1, 1, 0, c->fx_box.abgr, &aux );
                }
                break;

            /* Four radii and a ramp -- and still one surface, one command and no batch split,
               exactly like the uniform fill it generalizes. */
            case GUI_CMD_ROUND_RECT_EX:
                s_tess.cur_corner_pow = c->round_rect.corner_pow;
                tess_round_rect_ex( c->round_rect.x, c->round_rect.y,
                                    c->round_rect.w, c->round_rect.h,
                                    c->round_rect.rtl, c->round_rect.rtr,
                                    c->round_rect.rbr, c->round_rect.rbl,
                                    c->round_rect.feather, c->round_rect.abgr,
                                    c->round_rect.col_b, c->round_rect.grad_ang,
                                    c->round_rect.grad_kind, c->round_rect.grad_mid );
                break;

            /* The sectors share their geometry and differ only in the field the fragment
               evaluates: round caps on a band, sharp radial edges on a wedge.  A dashed sector is
               no longer a field of its own -- it is this same ARC under GUI_OP_DASH. */
            case GUI_CMD_ARC:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, c->arc.thickness,
                             c->arc.a0, c->arc.a1, GUI_FX_ARC, c->arc.abgr,
                             0.0f, 0.0f,
                             c->arc.spin_rate, c->arc.spin_phase,
                             c->arc.curve, c->arc.curve_param, c->arc.abgr );
                break;

            case GUI_CMD_PIE:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, 0.0f,
                             c->arc.a0, c->arc.a1, GUI_FX_PIE, c->arc.abgr,
                             0.0f, 0.0f,
                             c->arc.spin_rate, c->arc.spin_phase,
                             c->arc.curve, c->arc.curve_param, c->arc.abgr );
                break;

            case GUI_CMD_ARC_DASH:
                tess_fx_arc( c->arc_dash.cx, c->arc_dash.cy, c->arc_dash.r,
                             c->arc_dash.thickness, c->arc_dash.a0, c->arc_dash.a1,
                             GUI_FX_ARC, c->arc_dash.abgr,
                             c->arc_dash.period / TESS_TAU, c->arc_dash.duty,
                             0.0f, 0.0f, 0u, 0.0f, c->arc_dash.abgr );
                break;

            /* The sweep is the plain ARC plus GUI_OP_GRAD_ALONG: col_b rides the record and the
               fragment ramps on the sector's own arc-length coordinate. */
            case GUI_CMD_ARC_GRAD:
                tess_fx_arc( c->arc_grad.cx, c->arc_grad.cy, c->arc_grad.r,
                             c->arc_grad.thickness, c->arc_grad.a0, c->arc_grad.a1,
                             GUI_FX_ARC, c->arc_grad.col_b,
                             0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, c->arc_grad.col_a );
                break;

            /* The framebuffer-tiling patterns: the fragment does the tiling, the CPU's share is
               the quantized pitch + anchor phase (see tess_checker). */
            case GUI_CMD_CHECKER:
                s_tess.cur_corner_pow = c->checker.corner_pow;
                tess_checker( c->checker.x, c->checker.y, c->checker.w, c->checker.h,
                              c->checker.cell, c->checker.rounding,
                              c->checker.col_a, c->checker.col_b );
                break;

            case GUI_CMD_GRID:
                s_tess.cur_corner_pow = c->grid.corner_pow;
                tess_grid( c->grid.x, c->grid.y, c->grid.w, c->grid.h,
                           c->grid.ox, c->grid.oy, c->grid.cell, c->grid.thickness,
                           c->grid.angle, c->grid.rounding, c->grid.stripes != 0u,
                           c->grid.abgr );
                break;

            /* The regular polygon: filled, or stroked under GUI_OP_BAND -- the op set here for
               the reason every shape's is (the record's band width is sized from it). */
            case GUI_CMD_NGON:
                if ( c->ngon.thickness > 0.0f )
                    s_tess.cur_ops |= GUI_OP_BAND;
                tess_fx_ngon( c->ngon.cx, c->ngon.cy, c->ngon.r, c->ngon.sides,
                              c->ngon.rot, c->ngon.rounding, c->ngon.thickness,
                              c->ngon.star, c->ngon.abgr );
                break;

            /* The dashed border: a BAND box whose coverage the fragment cuts on the perimeter
               coordinate (GUI_OP_DASH).  The CPU's share is the SNAP: fit a whole number of
               dash cycles to the perimeter, computed from the same clamped radius the record
               will state, so the pattern meets itself where the walk closes. */
            case GUI_CMD_BOX_DASH:
            {
                f32 hw  = c->box_dash.w * 0.5f, hh = c->box_dash.h * 0.5f;
                f32 lim = ( hw < hh ) ? hw : hh;
                f32 r   = c->box_dash.rounding;
                if ( r > lim )  r = lim;
                if ( r < 0.0f ) r = 0.0f;

                f32 L      = 4.0f * ( hw + hh ) - 8.0f * r + TESS_TAU * r;
                f32 period = c->box_dash.dash + c->box_dash.gap;
                f32 n      = ( period > 0.0f ) ? floorf( L / period + 0.5f ) : 1.0f;
                if ( n < 1.0f ) n = 1.0f;

                tess_fx_aux_t aux = { 0 };
                aux.dash_period = L / n;
                aux.dash_duty   = c->box_dash.dash / period;
                aux.dash_scroll = 1.0f;                   /* the ants: one period per cycle */
                aux.anim_curve  = c->box_dash.curve;
                aux.anim_param  = c->box_dash.curve_param;

                /* The caller speaks in PERIMETER PX -- px/sec of scroll, a px offset.  The clock
                   speaks in cycles, the one unit every animating op reads.  Convert both once,
                   here, against the period the snap above just settled. */
                aux.anim_rate   = ( aux.dash_period > 0.0f )
                                ? c->box_dash.rate  / aux.dash_period : 0.0f;
                aux.anim_phase  = c->box_dash.anim_phase
                                + ( ( aux.dash_period > 0.0f )
                                    ? c->box_dash.phase / aux.dash_period : 0.0f );

                s_tess.cur_ops |= GUI_OP_BAND | GUI_OP_DASH;
                tess_fx_box( c->box_dash.x, c->box_dash.y, c->box_dash.w, c->box_dash.h,
                             c->box_dash.rounding, TESS_FX_AA, c->box_dash.t,
                             0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, c->box_dash.abgr, &aux );
                break;
            }

            /* One textured quad about its centre -- the glyph-run transform (tess_quad_xf)
               with the pivot every icon caller wants.  No snap, by the transformed-quad rule. */
            /* The lattice: one quad, one style record, however many copies -- the count reaches
               the fragment as the set's extent against the pitch, so it costs no lane and no
               per-copy work. */
            case GUI_CMD_REPEAT:
                tess_repeat_box( c->repeat.cx, c->repeat.cy, c->repeat.nx, c->repeat.ny,
                                 c->repeat.pitch_x, c->repeat.pitch_y,
                                 c->repeat.cell_w, c->repeat.cell_h,
                                 c->repeat.rounding, c->repeat.abgr,
                                 c->repeat.col_b, c->repeat.fill );
                break;

            /* The ring: same one-quad trade taken angularly, and at a non-zero rate it animates
               in the fragment -- so the command's bytes stay put while it moves. */
            case GUI_CMD_REPEAT_POLAR:
                tess_repeat_polar( c->repeat_polar.cx, c->repeat_polar.cy,
                                   c->repeat_polar.n, c->repeat_polar.orbit,
                                   c->repeat_polar.cell_w, c->repeat_polar.cell_h,
                                   c->repeat_polar.rounding, c->repeat_polar.rate,
                                   c->repeat_polar.phase, c->repeat_polar.curve,
                                   c->repeat_polar.curve_param, c->repeat_polar.abgr,
                                   c->repeat_polar.col_b );
                break;

            /* Subtraction: the box minus the second box the record states (GUI_OP_CUT_SHAPE). */
            case GUI_CMD_BOX_CUT:
                tess_box_cut( c->box_cut.x, c->box_cut.y, c->box_cut.w, c->box_cut.h,
                              c->box_cut.rounding, c->box_cut.cut_dx, c->box_cut.cut_dy,
                              c->box_cut.cut_w, c->box_cut.cut_h,
                              c->box_cut.cut_r, c->box_cut.cut_aa, c->box_cut.abgr );
                break;

            case GUI_CMD_IMAGE_XF:
            {
                f32 hx = c->image_xf.w * 0.5f, hy = c->image_xf.h * 0.5f;
                tess_quad_xf( c->image_xf.x + hx, c->image_xf.y + hy,
                              cosf( c->image_xf.rot ), sinf( c->image_xf.rot ),
                              -hx, -hy, c->image_xf.w, c->image_xf.h,
                              c->image_xf.u0, c->image_xf.v0, c->image_xf.u1, c->image_xf.v1,
                              c->image_xf.tex_idx, c->image_xf.abgr );
                break;
            }

            case GUI_CMD_TRIANGLE:
                tess_triangle( c->tri.ax, c->tri.ay, c->tri.bx, c->tri.by,
                               c->tri.cx, c->tri.cy, c->tri.abgr );
                break;

            case GUI_CMD_BEZIER:
                tess_bezier( c->bezier.ax, c->bezier.ay, c->bezier.cx, c->bezier.cy,
                             c->bezier.bx, c->bezier.by, c->bezier.thickness, c->bezier.abgr );
                break;

            /* The outline word is set once for the whole run: every glyph quad the loop emits
               carries it, and the fragment resolves fill and outline from the one distance field
               it was already sampling. */
            /* The only two cases that read glyphs, and therefore the only two that care which font
               is active.  Guarded on a change rather than set unconditionally because a run of
               labels in one font is the overwhelmingly common case and font_use rebuilds metrics. */
            case GUI_CMD_TEXT:
                if ( c->text.font != cur_font )
                    font_use( cur_font = c->text.font );
                tess_text_edge_prim( c->text.edge_w, c->text.edge_col );
                tess_text_n( c->text.x, c->text.y, c->text.abgr, s_draw.text_pool + c->text.off,
                             c->text.len, c->text.clip_x0, c->text.clip_x1 );
                break;

            /* Shadow + main copy in one string walk -- see tess_text_shadow_n. No TEXT_EDGE field
               to prime: cur_ops was just zeroed above, which is the no-edge state every plain
               run wants, and a drop shadow is never combined with a distance-field halo. */
            case GUI_CMD_TEXT_SHADOW:
                if ( c->text_shadow.font != cur_font )
                    font_use( cur_font = c->text_shadow.font );
                tess_text_shadow_n( c->text_shadow.x, c->text_shadow.y, c->text_shadow.abgr,
                                     c->text_shadow.shadow_abgr, c->text_shadow.dx, c->text_shadow.dy,
                                     s_draw.text_pool + c->text_shadow.off, c->text_shadow.len,
                                     c->text_shadow.clip_x0, c->text_shadow.clip_x1 );
                break;

            case GUI_CMD_TEXT_XF:
                if ( c->text_xf.font != cur_font )
                    font_use( cur_font = c->text_xf.font );
                tess_text_edge_prim( c->text_xf.edge_w, c->text_xf.edge_col );
                tess_text_xf( c->text_xf.x, c->text_xf.y, c->text_xf.abgr,
                              s_draw.text_pool + c->text_xf.off, c->text_xf.len,
                              c->text_xf.scale, c->text_xf.rot );
                break;

            /* Always a CAPSULE, because only diagonals ever arrive: gui_draw_line routes every
               axis-aligned segment through a grid-snapped rect at EMIT (stroke_axis_aligned_rect,
               gui_emit_path.c), and that is the sole producer of GUI_CMD_LINE.  One segment has
               no joints, which is the only thing that kept the ribbon (see tess_fx_segment). */
            case GUI_CMD_LINE:
                tess_fx_segment( c->line.x0, c->line.y0, c->line.x1, c->line.y1,
                                 c->line.thickness, c->line.border, c->line.abgr );
                break;

            case GUI_CMD_POLYLINE:
            {
                const gui_vec2_t* pts = &s_draw.points[ c->polyline.pt_offset ];
                f32 center_off = stroke_center_offset( c->polyline.align, c->polyline.thickness * 0.5f );
                tess_stroke_poly_aa( pts, c->polyline.pt_count, c->polyline.thickness,
                                     center_off, c->polyline.closed, c->polyline.abgr );
                break;
            }

            case GUI_CMD_DASHED_LINE:
                tess_dashed_line( c->dash.x0, c->dash.y0, c->dash.x1, c->dash.y1,
                                  c->dash.thickness, c->dash.period, c->dash.duty, c->dash.abgr );
                break;

            case GUI_CMD_RECT_GRADIENT:
                tess_rect_gradient( c->gradient.x, c->gradient.y, c->gradient.w, c->gradient.h,
                                    c->gradient.col_a, c->gradient.col_b, c->gradient.horizontal );
                break;

            case GUI_CMD_RECT_LIST:
            {
                /* One quad per pooled entry; all share this command's clip/vp so they collapse
                   into the same GPU batch.  Solid color (tex 0, self-sampled), never rounded. */
                const gui_rect_col_t* rl = &s_draw.rect_pool[ c->rect_list.offset ];
                for ( u32 k = 0; k < c->rect_list.count; ++k )
                    tess_rect_filled( rl[ k ].x, rl[ k ].y, rl[ k ].w, rl[ k ].h,
                                      0, 0, 1, 1, 0, rl[ k ].abgr );
                break;
            }

            case GUI_CMD_SPRITE:
                /* 1, 3 or 9 quads from this one command -- the whole expansion, plus the sprite
                   lookup it needs, lives in tess_sprite. */
                tess_sprite( c );
                break;
        }
    }

    if ( open_vid != GUI_ID_NONE )
        volatile_range_close( open_vid, vb_open, pb_open, cmd_open );

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/