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
   RANGE's quads / prim records / GPU commands are fully written; it records the block's slot-relative
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
   the following quads' prim records.  A bitmap label, an SDF heading and the fill behind them still go out as one
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

        /* The style answer this command site parked last time, if its bytes have not moved
           since (gui_render_intern.c, pal_cmd_hint).  One u32 compare against a hash the emit
           phase already folded; tess_prim_local confirms whatever comes back before using it.
           */
        s_tess.cmd_hint       = pal_cmd_hint( ci );
        s_tess.cmd_prim_out  = 0u;    /* an arena index: "nothing resolved yet" */

        /* The op word is ambient over ONE command and cleared here, so a case that sets it
           cannot leak the effect onto the next primitive.  That containment is the whole reason
           it can be ambient at all -- it lets an outline reach every glyph of a run without
           threading a parameter through tess_rect_filled, which every fill in the library
           shares. */
        s_tess.cur_ops        = 0u;
        s_tess.cur_corner_pow = 0.0f;
        s_tess.cur_fx_field   = 0u;
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
            case GUI_CMD_RECT_FILL:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->rect_fill.rounding > 0.0f )
                {
                    s_tess.cur_corner_pow = e->rect_fill.corner_pow;
                    tess_fx_box( e->rect_fill.x, e->rect_fill.y, e->rect_fill.w, e->rect_fill.h,
                                 e->rect_fill.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                                 0, 0, 1, 1,
                                 0, e->rect_fill.abgr, NULL );
                }
                else
                    tess_rect_filled( e->rect_fill.x, e->rect_fill.y, e->rect_fill.w, e->rect_fill.h,
                                      0, 0, 1, 1,
                                      0, e->rect_fill.abgr );
                break;
            }

            case GUI_CMD_RECT_TEX:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->rect_tex.rounding > 0.0f )
                {
                    s_tess.cur_corner_pow = e->rect_tex.corner_pow;
                    tess_fx_box( e->rect_tex.x, e->rect_tex.y, e->rect_tex.w, e->rect_tex.h,
                                 e->rect_tex.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                                 e->rect_tex.u0, e->rect_tex.v0, e->rect_tex.u1, e->rect_tex.v1,
                                 e->rect_tex.tex_idx, e->rect_tex.abgr, NULL );
                }
                else
                    tess_rect_filled( e->rect_tex.x, e->rect_tex.y, e->rect_tex.w, e->rect_tex.h,
                                      e->rect_tex.u0, e->rect_tex.v0, e->rect_tex.u1, e->rect_tex.v1,
                                      e->rect_tex.tex_idx, e->rect_tex.abgr );
                break;
            }

            /* The band measures from the OUTER boundary inward, matching the square path's
               INSIDE band (and the closed AA stroke this replaced). */
            case GUI_CMD_RECT_OUTLINE:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->rect_outline.rounding > 0.0f )
                {
                    s_tess.cur_ops       |= GUI_OP_BAND;
                    s_tess.cur_corner_pow = e->rect_outline.corner_pow;
                    tess_fx_box( e->rect_outline.x, e->rect_outline.y,
                                 e->rect_outline.w, e->rect_outline.h,
                                 e->rect_outline.rounding, TESS_FX_AA, e->rect_outline.t,
                                 0.0f, 0.0f, 0.0f,
                                 0, 0, 1, 1, 0, e->rect_outline.abgr, NULL );
                }
                else
                    tess_rect_outline( e->rect_outline.x, e->rect_outline.y,
                                       e->rect_outline.w, e->rect_outline.h,
                                       e->rect_outline.t, e->rect_outline.abgr );
                break;
            }

            /* Body + border in one surface.  A square frame runs the field with feather 0 
               -- a hard cut on the snapped boundary, matching the crisp edges the 
               fill + four-rail pair drew -- and a rounded one takes the standard AA band. */
            case GUI_CMD_FRAME:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                s_tess.cur_ops       |= GUI_OP_FRAME;
                s_tess.cur_corner_pow = e->frame.corner_pow;
                s_tess.cur_col_border = e->frame.col_border;   /* rides the quad, not the style */
                tess_fx_box( e->frame.x, e->frame.y, e->frame.w, e->frame.h, e->frame.rounding,
                           ( e->frame.rounding > 0.0f ) ? TESS_FX_AA : 0.0f,
                             e->frame.t, 0.0f, 0.0f, 0.0f,
                             0, 0, 1, 1, /* tex id */ 0, e->frame.abgr, NULL );
                break;
            }

            /* frame's per-corner sibling -- same body + border composite, four radii instead of
               one.  tess_round_frame_ex sets GUI_OP_FRAME itself (the round_rect_ex convention:
               the per-corner "_ex" helpers own their op, where the uniform ones take it from the
               dispatcher). */
            case GUI_CMD_ROUND_FRAME_EX:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                s_tess.cur_corner_pow = e->round_frame_ex.corner_pow;
                s_tess.cur_col_border = e->round_frame_ex.col_border;
                tess_round_frame_ex( e->round_frame_ex.x, e->round_frame_ex.y,
                                     e->round_frame_ex.w, e->round_frame_ex.h,
                                     e->round_frame_ex.rtl, e->round_frame_ex.rtr,
                                     e->round_frame_ex.rbr, e->round_frame_ex.rbl,
                                     e->round_frame_ex.t, e->round_frame_ex.abgr );
                break;
            }

            /* The parameterized surface: a shadow is the wide feather (what used to be six
               stacked rects pretending to be a gaussian is the exact same falloff the corners
               use, only spread out), a pulse the shader-clock word -- geometrically a plain
               rounded fill whose record is correct for every frame it runs, so the retained
               slot never invalidates and the breathing costs no re-tessellation. */
            case GUI_CMD_FX_BOX:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                /* All of these are the one GUI_FX_BOX mode; the variant, the rate and the swell
                   pick which ops ride the tex word.  They are INDEPENDENT flags rather than a
                   choice of one, which is what lets a cut or inset surface breathe -- as a mode
                   number the pulse had to displace whichever shape it was applied to.  Set before
                   the tessellator runs because the interior hole is sized from them (see
                   `reach`). */
                if ( e->fx_box.variant == GUI_FX_BOX_SKIRT )   s_tess.cur_ops |= GUI_OP_CUT;
                if ( e->fx_box.variant == GUI_FX_BOX_INSET )   s_tess.cur_ops |= GUI_OP_INSET;
                if ( e->fx_box.variant == GUI_FX_BOX_GLOW )    s_tess.cur_ops |= GUI_OP_GLOW;
                if ( e->fx_box.variant == GUI_FX_BOX_RING )    s_tess.cur_ops |= GUI_OP_BAND;
                if ( e->fx_box.rate    > 0.0f )  s_tess.cur_ops |= GUI_OP_PULSE;
                if ( e->fx_box.swell  != 0.0f )  s_tess.cur_ops |= GUI_OP_SWELL;
                {
                    /* The cut boundary, for the DIRECTIONAL cast: the command states where the
                       shadow is drawn, and this says where the caster it belongs to sits relative
                       to it.  Zero for every other variant, and the aux is read only under the op
                       that owns it. */
                    tess_fx_aux_t aux = { 0 };
                    aux.cut_dx     = e->fx_box.cut_dx;
                    aux.cut_dy     = e->fx_box.cut_dy;
                    aux.anim_phase = e->fx_box.phase;
                    aux.anim_curve = e->fx_box.curve;
                    aux.anim_param = e->fx_box.curve_param;
                    aux.swell_amp  = e->fx_box.swell;
                    s_tess.cur_corner_pow = e->fx_box.corner_pow;

                    /* A BAKED shape takes the place of the analytic box: same surface, same ops,
                       its distance sampled from the SDF atlas instead of evaluated.  The uv is
                       resolved HERE rather than at emit because an atlas repack moves the tenant
                       under geometry the retained cache may hold for many frames -- the sprite's
                       rule, and res_sdf_generation folds into the window hash to force the
                       re-resolve (gui_build_diff.c).  The rect the command carries is already the
                       PADDED box, so the uv spans the tenant whole and the skirt reaches the
                       margin every effect travels through. */
                    f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                    u32 tex = 0u;
                    if ( e->fx_box.shape != GUI_SHAPE_NONE )
                    {
                        tex = shape_tex( e->fx_box.shape );
                        if ( tex == 0u || !shape_uv( e->fx_box.shape, &u0, &v0, &u1, &v1 ) )
                            break;   /* atlas not up yet -- skip the quad, as a glyph run does */

                        s_tess.cur_fx_field = (u32)GUI_FX_TEX;

                        /* Both of these walk the field's BOUNDARY COORDINATE, which a stored
                           distance field does not have -- there is no arc length in an R8 texel.
                           Dropped rather than left to cut on a coordinate that is always zero, so
                           the shape draws right and the mistake is stated once. */
                        if ( s_tess.cur_ops & ( GUI_OP_DASH | GUI_OP_GRAD_ALONG ) )
                        {
                            static bool warned;
                            s_tess.cur_ops &= ~( GUI_OP_DASH | GUI_OP_GRAD_ALONG );
                            if ( !warned )
                            {
                                warned = true;
                                gui_log( GUI_LOG_WARN,
                                         "DASH / GRAD_ALONG dropped on a baked shape -- a sampled "
                                         "field states no boundary coordinate to walk" );
                            }
                        }
                    }

                    tess_fx_box( e->fx_box.x, e->fx_box.y, e->fx_box.w, e->fx_box.h,
                                 e->fx_box.rounding, e->fx_box.feather, e->fx_box.border,
                                 e->fx_box.rate, e->fx_box.depth, e->fx_box.rot,
                                 u0, v0, u1, v1, tex, e->fx_box.abgr, &aux );
                }
                break;
            }

            /* Four radii, a ramp, and an optional border -- and still one surface, one command
               and no batch split, exactly like the uniform fill/outline it generalizes. */
            case GUI_CMD_ROUND_RECT_EX:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                s_tess.cur_corner_pow = e->round_rect.corner_pow;
                tess_round_rect_ex( e->round_rect.x, e->round_rect.y,
                                    e->round_rect.w, e->round_rect.h,
                                    e->round_rect.rtl, e->round_rect.rtr,
                                    e->round_rect.rbr, e->round_rect.rbl,
                                    e->round_rect.feather, e->round_rect.border,
                                    e->round_rect.abgr,
                                    e->round_rect.col_b, e->round_rect.grad_ang,
                                    e->round_rect.grad_kind, e->round_rect.grad_mid );
                break;
            }

            /* The sectors share their geometry and differ only in the field the fragment
               evaluates: round caps on a band, sharp radial edges on a wedge.  A dashed sector is
               no longer a field of its own -- it is this same ARC under GUI_OP_DASH. */
            case GUI_CMD_ARC:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_fx_arc( e->arc.cx, e->arc.cy, e->arc.r, e->arc.thickness,
                             e->arc.a0, e->arc.a1, GUI_FX_ARC, e->arc.abgr,
                             0.0f, 0.0f,
                             e->arc.spin_rate, e->arc.spin_phase,
                             e->arc.curve, e->arc.curve_param, e->arc.abgr,
                             e->arc.flat_caps );
                break;
            }

            case GUI_CMD_PIE:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_fx_arc( e->arc.cx, e->arc.cy, e->arc.r, 0.0f,
                             e->arc.a0, e->arc.a1, GUI_FX_PIE, e->arc.abgr,
                             0.0f, 0.0f,
                             e->arc.spin_rate, e->arc.spin_phase,
                             e->arc.curve, e->arc.curve_param, e->arc.abgr,
                             false );
                break;
            }

            case GUI_CMD_ARC_DASH:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_fx_arc( e->arc_dash.cx, e->arc_dash.cy, e->arc_dash.r,
                             e->arc_dash.thickness, e->arc_dash.a0, e->arc_dash.a1,
                             GUI_FX_ARC, e->arc_dash.abgr,
                             e->arc_dash.period / TESS_TAU, e->arc_dash.duty,
                             0.0f, 0.0f, 0u, 0.0f, e->arc_dash.abgr,
                             e->arc_dash.flat_caps );
                break;
            }

            /* The sweep is the plain ARC plus GUI_OP_GRAD_ALONG: col_b rides the record and the
               fragment ramps on the sector's own arc-length coordinate. */
            case GUI_CMD_ARC_GRAD:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_fx_arc( e->arc_grad.cx, e->arc_grad.cy, e->arc_grad.r,
                             e->arc_grad.thickness, e->arc_grad.a0, e->arc_grad.a1,
                             GUI_FX_ARC, e->arc_grad.col_b,
                             0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, e->arc_grad.col_a,
                             e->arc_grad.flat_caps );
                break;
            }

            /* The framebuffer-tiling patterns: the fragment does the tiling, the CPU's share is
               the quantized pitch + anchor phase (see tess_checker). */
            case GUI_CMD_CHECKER:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                s_tess.cur_corner_pow = e->checker.corner_pow;
                tess_checker( e->checker.x, e->checker.y, e->checker.w, e->checker.h,
                              e->checker.cell, e->checker.rounding,
                              e->checker.col_a, e->checker.col_b );
                break;
            }

            case GUI_CMD_GRID:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                s_tess.cur_corner_pow = e->grid.corner_pow;
                tess_grid( e->grid.x, e->grid.y, e->grid.w, e->grid.h,
                           e->grid.ox, e->grid.oy, e->grid.cell, e->grid.thickness,
                           e->grid.angle, e->grid.rounding, e->grid.stripes != 0u,
                           e->grid.abgr );
                break;
            }

            /* The regular polygon: filled, or stroked under GUI_OP_BAND -- the op set here for
               the reason every shape's is (the record's band width is sized from it). */
            case GUI_CMD_NGON:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->ngon.thickness > 0.0f )
                    s_tess.cur_ops |= GUI_OP_BAND;
                tess_fx_ngon( e->ngon.cx, e->ngon.cy, e->ngon.r, e->ngon.sides,
                              e->ngon.rot, e->ngon.rounding, e->ngon.thickness,
                              e->ngon.star, e->ngon.abgr );
                break;
            }

            /* The dashed border: a BAND box whose coverage the fragment cuts on the perimeter
               coordinate (GUI_OP_DASH).  The CPU's share is the SNAP: fit a whole number of
               dash cycles to the perimeter, computed from the same clamped radius the record
               will state, so the pattern meets itself where the walk closes. */
            case GUI_CMD_BOX_DASH:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                f32 hw  = e->box_dash.w * 0.5f, hh = e->box_dash.h * 0.5f;
                f32 lim = ( hw < hh ) ? hw : hh;
                f32 r   = e->box_dash.rounding;
                if ( r > lim )  r = lim;
                if ( r < 0.0f ) r = 0.0f;

                f32 L      = 4.0f * ( hw + hh ) - 8.0f * r + TESS_TAU * r;
                f32 period = e->box_dash.dash + e->box_dash.gap;
                f32 n      = ( period > 0.0f ) ? floorf( L / period + 0.5f ) : 1.0f;
                if ( n < 1.0f ) n = 1.0f;

                tess_fx_aux_t aux = { 0 };
                aux.dash_period = L / n;
                aux.dash_duty   = e->box_dash.dash / period;
                aux.dash_scroll = 1.0f;                   /* the ants: one period per cycle */
                aux.anim_curve  = e->box_dash.curve;
                aux.anim_param  = e->box_dash.curve_param;

                /* The caller speaks in PERIMETER PX -- px/sec of scroll, a px offset.  The clock
                   speaks in cycles, the one unit every animating op reads.  Convert both once,
                   here, against the period the snap above just settled. */
                aux.anim_rate   = ( aux.dash_period > 0.0f )
                                ? e->box_dash.rate  / aux.dash_period : 0.0f;
                aux.anim_phase  = e->box_dash.anim_phase
                                + ( ( aux.dash_period > 0.0f )
                                    ? e->box_dash.phase / aux.dash_period : 0.0f );

                s_tess.cur_ops |= GUI_OP_BAND | GUI_OP_DASH;
                tess_fx_box( e->box_dash.x, e->box_dash.y, e->box_dash.w, e->box_dash.h,
                             e->box_dash.rounding, TESS_FX_AA, e->box_dash.t,
                             0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, e->box_dash.abgr, &aux );
                break;
            }

            /* One textured quad about its centre -- the glyph-run transform (tess_quad_xf)
               with the pivot every icon caller wants.  No snap, by the transformed-quad rule. */
            /* The lattice: one quad, one prim record, however many copies -- the count reaches
               the fragment as the set's extent against the pitch, so it costs no lane and no
               per-copy work. */
            case GUI_CMD_REPEAT:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_repeat_box( e->repeat.cx, e->repeat.cy, e->repeat.nx, e->repeat.ny,
                                 e->repeat.pitch_x, e->repeat.pitch_y,
                                 e->repeat.cell_w, e->repeat.cell_h,
                                 e->repeat.rounding, e->repeat.abgr,
                                 e->repeat.col_b, e->repeat.fill );
                break;
            }

            /* The ring: same one-quad trade taken angularly, and at a non-zero rate it animates
               in the fragment -- so the command's bytes stay put while it moves. */
            case GUI_CMD_REPEAT_POLAR:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_repeat_polar( e->repeat_polar.cx, e->repeat_polar.cy,
                                   e->repeat_polar.n, e->repeat_polar.orbit,
                                   e->repeat_polar.cell_w, e->repeat_polar.cell_h,
                                   e->repeat_polar.rounding, e->repeat_polar.rate,
                                   e->repeat_polar.phase, e->repeat_polar.curve,
                                   e->repeat_polar.curve_param, e->repeat_polar.abgr,
                                   e->repeat_polar.col_b );
                break;
            }

            /* Subtraction: the box minus the second box the record states (GUI_OP_CUT_SHAPE). */
            case GUI_CMD_BOX_CUT:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_box_cut( e->box_cut.x, e->box_cut.y, e->box_cut.w, e->box_cut.h,
                              e->box_cut.rounding, e->box_cut.cut_dx, e->box_cut.cut_dy,
                              e->box_cut.cut_w, e->box_cut.cut_h,
                              e->box_cut.cut_r, e->box_cut.cut_aa, e->box_cut.abgr );
                break;
            }

            case GUI_CMD_IMAGE_XF:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                f32 hx = e->image_xf.w * 0.5f, hy = e->image_xf.h * 0.5f;
                tess_quad_xf( e->image_xf.x + hx, e->image_xf.y + hy,
                              cosf( e->image_xf.rot ), sinf( e->image_xf.rot ),
                              -hx, -hy, e->image_xf.w, e->image_xf.h,
                              e->image_xf.u0, e->image_xf.v0, e->image_xf.u1, e->image_xf.v1,
                              e->image_xf.tex_idx, e->image_xf.abgr );
                break;
            }

            case GUI_CMD_TRIANGLE:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_triangle( e->tri.ax, e->tri.ay, e->tri.bx, e->tri.by,
                               e->tri.cx, e->tri.cy, e->tri.abgr );
                break;
            }

            case GUI_CMD_BEZIER:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_bezier( e->bezier.ax, e->bezier.ay, e->bezier.cx, e->bezier.cy,
                             e->bezier.bx, e->bezier.by, e->bezier.thickness, e->bezier.abgr );
                break;
            }

            /* The outline word is set once for the whole run: every glyph quad the loop emits
               carries it, and the fragment resolves fill and outline from the one distance field
               it was already sampling. */
            /* The only two cases that read glyphs, and therefore the only two that care which font
               is active.  Guarded on a change rather than set unconditionally because a run of
               labels in one font is the overwhelmingly common case and font_use rebuilds metrics. */
            case GUI_CMD_TEXT:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->text.font != cur_font )
                    font_use( cur_font = e->text.font );
                tess_text_edge_prim( e->text.edge_w, e->text.edge_col );
                tess_text_n( e->text.x, e->text.y, e->text.abgr, s_draw.text_pool + e->text.off,
                             e->text.len, e->text.clip_x0, e->text.clip_x1 );
                break;
            }

            /* Shadow + main copy in one string walk -- see tess_text_shadow_n. No TEXT_EDGE field
               to prime: cur_ops was just zeroed above, which is the no-edge state every plain
               run wants, and a drop shadow is never combined with a distance-field halo. */
            case GUI_CMD_TEXT_SHADOW:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->text_shadow.font != cur_font )
                    font_use( cur_font = e->text_shadow.font );
                tess_text_shadow_n( e->text_shadow.x, e->text_shadow.y, e->text_shadow.abgr,
                                     e->text_shadow.shadow_abgr, e->text_shadow.dx, e->text_shadow.dy,
                                     s_draw.text_pool + e->text_shadow.off, e->text_shadow.len,
                                     e->text_shadow.clip_x0, e->text_shadow.clip_x1 );
                break;
            }

            case GUI_CMD_TEXT_XF:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                if ( e->text_xf.font != cur_font )
                    font_use( cur_font = e->text_xf.font );
                tess_text_edge_prim( e->text_xf.edge_w, e->text_xf.edge_col );
                tess_text_xf( e->text_xf.x, e->text_xf.y, e->text_xf.abgr,
                              s_draw.text_pool + e->text_xf.off, e->text_xf.len,
                              e->text_xf.scale, e->text_xf.rot );
                break;
            }

            /* Always a CAPSULE, because only diagonals ever arrive: gui_draw_line routes every
               axis-aligned segment through a grid-snapped rect at EMIT (stroke_axis_aligned_rect,
               gui_emit_path.c), and that is the sole producer of GUI_CMD_LINE.  One segment has
               no joints, which is the only thing that kept the ribbon (see tess_fx_segment). */
            case GUI_CMD_LINE:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_fx_segment( e->line.x0, e->line.y0, e->line.x1, e->line.y1,
                                 e->line.thickness, e->line.border, e->line.abgr );
                break;
            }

            case GUI_CMD_POLYLINE:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                const gui_vec2_t* pts = &s_draw.points[ e->polyline.pt_offset ];
                f32 center_off = stroke_center_offset( e->polyline.align, e->polyline.thickness * 0.5f );
                tess_stroke_poly_aa( pts, e->polyline.pt_count, e->polyline.thickness,
                                     center_off, e->polyline.closed, e->polyline.abgr );
                break;
            }

            case GUI_CMD_DASHED_LINE:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_dashed_line( e->dash.x0, e->dash.y0, e->dash.x1, e->dash.y1,
                                  e->dash.thickness, e->dash.period, e->dash.duty, e->dash.abgr );
                break;
            }

            case GUI_CMD_RECT_GRADIENT:
            {
                const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
                tess_rect_gradient( e->gradient.x, e->gradient.y, e->gradient.w, e->gradient.h,
                                    e->gradient.col_a, e->gradient.col_b, e->gradient.horizontal );
                break;
            }

            case GUI_CMD_RECT_LIST:
            {
                /* One quad per pooled entry; all share this command's clip/vp so they collapse
                   into the same GPU batch.  Solid color (tex 0, self-sampled), never rounded. */
                const gui_cmd_ext_t*  e  = draw_cmd_ext_slot( c->offset );
                const gui_rect_col_t* rl = &s_draw.rect_pool[ e->rect_list.offset ];
                for ( u32 k = 0; k < e->rect_list.count; ++k )
                    tess_rect_filled( rl[ k ].x, rl[ k ].y, rl[ k ].w, rl[ k ].h,
                                      0, 0, 1, 1, 0, rl[ k ].abgr );
                break;
            }

            case GUI_CMD_SPRITE:
                /* 1, 3 or 9 quads from this one command -- the whole expansion, plus the sprite
                   lookup it needs, lives in tess_sprite. */
                tess_sprite( draw_cmd_ext_slot( c->offset ) );
                break;
        }

        /* Park what this command resolved, for the next pass over it.  A command that commits
           several styles parks the LAST -- as much as one hint can hold -- and one that
           commits none (a plain glyph run resolves no style at all) parks nothing. */
        pal_cmd_learn( ci, s_tess.cmd_prim_out );
    }

    if ( open_vid != GUI_ID_NONE )
        volatile_range_close( open_vid, vb_open, pb_open, cmd_open );

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/