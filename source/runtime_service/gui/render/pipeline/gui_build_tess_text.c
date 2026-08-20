/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess_text.c -- patterns, text, dashed lines.

    Part 6 of 7 of the CPU-side quad-record builder (see gui_build_tess_state.c for the family
    overview).  Holds the framebuffer-tiling patterns (tess_checker, tess_grid, tess_pattern_push),
    the four text tessellators (tess_text_n, tess_text_shadow_n, tess_text_xf and its tess_quad_xf
    / tess_glyph_xf placement helpers, tess_text_edge_prim), and tess_dashed_line.

    Included right after gui_build_tess_arc.c (needs tess_fx_box, tess_quad_push,
    tess_rect_filled, tess_rect_glyph, tess_snap_px, tess_clamp_cell).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    tess_checker / tess_grid -- the framebuffer-tiling pattern quads.

    ONE quad each: the fragment computes the pattern from gl_FragCoord / SV_Position, not from
    the effect coordinate, and the reason is precision where these shapes actually live.  A
    backdrop is the one shape that reaches fullscreen, and there the HALF2 coordinate's ulp is a
    full pixel at the far corners -- a fine lattice line would land half a pixel wrong and blur.
    The rasterizer's own pixel coordinate is exact everywhere at any size.  (It also means the
    pattern assumes the pixel-space ortho mvp, which is the only mvp this pipeline has.)

    The CPU's share is the ANCHOR: quantize the cell pitch EXACTLY as the packed word carries it
    (1/4 px), then derive the phase against that quantized pitch -- deriving it against the raw
    pitch would let phase and pitch disagree by up to 1/8 px per cell, which walks the pattern
    off its anchor across a wide panel.  The checker's phase is a fraction of the TWO-cell
    colour period (one cell of phase would swap the colours); the grid's is a fraction of one
    cell.  Both ride the style record's pattern row (gui_prim_t, pat_phase).
==============================================================================================*/

/* A pattern quad, with the shape it lands in.  A zero radius is the plain rectangle the pattern
   used to be able to be and nothing else: one bare quad, no field, no falloff.  A non-zero radius
   routes the same record through the BOX field, so the lattice or the chequerboard is cut to a
   rounded boundary -- the whole point of these being ops rather than fields.  Either way it is ONE
   quad and one style. */
static void
tess_pattern_push( f32 x, f32 y, f32 w, f32 h, f32 rounding, u32 abgr )
{
    if ( rounding > 0.0f )
    {
        tess_fx_box( x, y, w, h, rounding, TESS_FX_AA, 0.0f,
                     0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, abgr, NULL );
        return;
    }
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}

static void
tess_checker( f32 x, f32 y, f32 w, f32 h, f32 cell, f32 rounding, u32 col_a, u32 col_b )
{
    /* Snap like tess_rect_filled: the pattern anchors at the box origin, so the box must land
       where the plain fill under it does. */
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_clamp_cell( cell );

    f32 period = 2.0f * cell;
    f32 phx    = ( x - period * floorf( x / period ) ) / period;
    f32 phy    = ( y - period * floorf( y / period ) ) / period;

    s_tess.cur_prim.pat_cell  = cell;
    s_tess.cur_prim.pat_phase = gui_uv_pack( phx, phy );
    s_tess.cur_prim.pat_col   = col_b;
    s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_CHECKER;

    tess_pattern_push( x, y, w, h, rounding, col_a );
}

static void
tess_grid( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 cell, f32 thickness,
           f32 angle, f32 rounding, bool stripes, u32 abgr )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_clamp_cell( cell );

    /* WRAP rather than clamp: a lattice at `angle` and at angle + pi are the same lattice, so an
       animated rotation must roll over rather than stick at pi, which is what a clamp would do.
       The wrapped value is used for BOTH the packed word and the phase below -- the fragment
       rotates the pixel coordinate by exactly this angle, so a disagreement would slide the
       pattern off its anchor. */
    angle -= TESS_PI * floorf( angle / TESS_PI );

    /* The lattice anchor, mod the quantized pitch.  (ox, oy) is a screen-space content origin
       and may be anywhere (a panned canvas sends large negatives); only its residue matters.
       The anchor is rotated INTO lattice space first, because that is the space the fragment
       does its mod in -- rotation is linear, so R(px - o) is R(px) - R(o), and the phase is the
       residue of R(o).  Taking the residue before the rotation would anchor the wrong point. */
    f32 acs = cosf( angle ), asn = sinf( angle );
    f32 rx  =  ox * acs + oy * asn;
    f32 ry  = -ox * asn + oy * acs;

    f32 phx = ( rx - cell * floorf( rx / cell ) ) / cell;
    f32 phy = ( ry - cell * floorf( ry / cell ) ) / cell;

    s_tess.cur_prim.pat_cell  = cell;
    s_tess.cur_prim.pat_size  = thickness;
    s_tess.cur_prim.pat_angle = angle;
    s_tess.cur_prim.pat_phase = gui_uv_pack( phx, phy );
    if ( stripes )
        s_tess.cur_ops |= GUI_OP_STRIPES;
    s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_GRID;

    tess_pattern_push( x, y, w, h, rounding, abgr );
}

/* The ambient TEXT_EDGE, straight onto the record: a band `width` px outside the glyph boundary,
   painted in `abgr`.  A zero width is no edge at all and leaves the field NONE, which is what
   every plain run wants. */
static void
tess_text_edge_prim( f32 width, u32 abgr )
{
    if ( width <= 0.0f )
        return;

    s_tess.cur_prim.pat_size = width;
    s_tess.cur_prim.pat_col  = abgr;
    s_tess.cur_ops |= GUI_OP_TEXT_EDGE;
}

/* Tessellate a glyph run from the font atlas into s_tess, hard-clipped to the horizontal pixel
   window [clip_x0, clip_x1].  Glyphs fully outside the window are skipped; glyphs fully inside emit
   whole; the (at most two) straddling glyphs are cut on a pixel boundary with their U remapped by
   the same fraction -- exact, since the glyph quad is an axis-aligned 1:1 atlas sample.  The window
   is monotonic with the left-to-right cursor, so interior glyphs pay only one compare: no clip math.
   The unclipped sentinel (clip_x1 >= GUI_TEXT_NO_CLIP) takes the original whole-run fast path. */
static void
tess_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    bool clipped = ( clip_x1 < GUI_TEXT_NO_CLIP );
    f32  cx      = x;

    /* Hoisted: the active font cannot change mid-run, and this carries the sampling model, so it is
       also what keeps a distance-field run in its own batch without the batcher knowing why. */
    u32  tex = font_tex();
    if ( tex == 0 )
        return;                       /* the font's atlas is not up yet -- nothing to sample */

    s_tess.cur_is_text = true;        /* every quad below is a character (glyph attribution) */
    s_tess.slot_text_runs++;

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        /* ID + placement in one record lookup; the atlas rect is the glyph table's now. */
        u32 gid;
        f32 ox, oy, gw, gh, advance;
        font_glyph_placed( cp, &gid, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
        {
            f32 gx0 = cx + ox;          /* glyph bitmap left/right in screen px */
            f32 gx1 = gx0 + gw;

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                /* Whole glyph (or no clipping): emit by table ID -- the hot interior path. */
                tess_rect_glyph( gx0, y + oy, gw, gh, gid, tex, abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                /* Straddler: cut to the window and walk U by the same fraction on each cut edge,
                   so the narrowed rect samples exactly the visible part of the glyph bitmap.  The
                   narrowed span is per-instance and has no table entry, so this one glyph pays the
                   second lookup and bakes its own rect -- at most two per run, at its ends. */
                f32 u0, v0, u1, v1, sox, soy, sgw, sgh, sadv;
                font_glyph( cp, &u0, &v0, &u1, &v1, &sox, &soy, &sgw, &sgh, &sadv );

                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                if ( nx0 < clip_x0 )    /* left edge cut  */
                {
                    nu0 = u0 + du * ( ( clip_x0 - gx0 ) / gw );
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )    /* right edge cut */
                {
                    nu1 = u0 + du * ( ( clip_x1 - gx0 ) / gw );
                    nx1 = clip_x1;
                }
                tess_rect_filled( nx0, y + oy, nx1 - nx0, gh, nu0, v0, nu1, v1, tex, abgr );
            }
            /* else: glyph wholly outside the window -- drop it. */
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )   /* cursor past the window: nothing further is visible */
            break;
    }

    s_tess.cur_is_text = false;
}

/* Same walk as tess_text_n, but each glyph decode + atlas lookup feeds TWO quads: the shadow
   copy (offset dx, dy; shadow_abgr) then the main glyph, in that order so the main glyph's
   antialiased edge composites over the shadow rather than under it.  Whichever of the pair a
   glyph resolves to (whole-glyph table id vs. cut-and-remapped rect) is decided once and used for
   both copies -- the shadow is never independently clip-tested, so the pair always lives or dies
   together instead of a shadow surviving a main glyph the clip window dropped (or the reverse). */
static void
tess_text_shadow_n( f32 x, f32 y, u32 abgr, u32 shadow_abgr, f32 dx, f32 dy,
                     const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    bool clipped = ( clip_x1 < GUI_TEXT_NO_CLIP );
    f32  cx      = x;

    u32 tex = font_tex();
    if ( tex == 0 )
        return;

    s_tess.cur_is_text = true;
    s_tess.slot_text_runs++;

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        u32 gid;
        f32 ox, oy, gw, gh, advance;
        font_glyph_placed( cp, &gid, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
        {
            f32 gx0 = cx + ox;
            f32 gx1 = gx0 + gw;

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                tess_rect_glyph( gx0 + dx, y + oy + dy, gw, gh, gid, tex, shadow_abgr );
                tess_rect_glyph( gx0,      y + oy,      gw, gh, gid, tex, abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                f32 u0, v0, u1, v1, sox, soy, sgw, sgh, sadv;
                font_glyph( cp, &u0, &v0, &u1, &v1, &sox, &soy, &sgw, &sgh, &sadv );

                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                if ( nx0 < clip_x0 )
                {
                    nu0 = u0 + du * ( ( clip_x0 - gx0 ) / gw );
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )
                {
                    nu1 = u0 + du * ( ( clip_x1 - gx0 ) / gw );
                    nx1 = clip_x1;
                }
                tess_rect_filled( nx0 + dx, y + oy + dy, nx1 - nx0, gh, nu0, v0, nu1, v1, tex,
                                   shadow_abgr );
                tess_rect_filled( nx0,      y + oy,      nx1 - nx0, gh, nu0, v0, nu1, v1, tex,
                                   abgr );
            }
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )
            break;
    }

    s_tess.cur_is_text = false;
}

/* One textured quad placed by an affine map: the local rect (lx, ly, lw, lh) is rotated by the
   prebuilt (cs, sn) and translated to the run origin (px, py) -- centre mapped through the
   transform, half-extents stored true, the style's rot pair doing the turn in the vertex stage.
   One style per (angle x scale) run: every glyph of a transformed run shares it.
   One thing this does NOT do: SNAP.  tess_rect_filled floors the origin to the pixel grid so
   straight edges stay crisp, which is right for chrome and wrong here twice over.  Snapping only
   the origin of a rotated quad moves the whole shape without straightening anything, and
   snapping a scaled run's per-glyph origins quantizes the advances -- the pen drifts by up to
   half a pixel per glyph and the word visibly breathes as the scale animates.  A transformed run
   is sub-pixel by nature; the distance field is what makes that legible (gui.h, GUI_TEX_SDF). */
static void
tess_quad_xf( f32 px, f32 py, f32 cs, f32 sn,
              f32 lx, f32 ly, f32 lw, f32 lh,
              f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr )
{
    f32 ccx = lx + lw * 0.5f, ccy = ly + lh * 0.5f;
    s_tess.cur_rot_c = cs;
    s_tess.cur_rot_s = sn;
    tess_quad_push( px + ccx * cs - ccy * sn, py + ccx * sn + ccy * cs,
                    lw * 0.5f, lh * 0.5f, GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr,
                    GUI_GLYPH_ID_NONE );
}

/* tess_quad_xf for a glyph: the same affine placement, the atlas rect named by table ID.  A
   transformed run is never cut mid-glyph (it has no window-relative pen to test), so every glyph
   of one takes this path. */
static void
tess_glyph_xf( f32 px, f32 py, f32 cs, f32 sn,
               f32 lx, f32 ly, f32 lw, f32 lh, u32 glyph_id, u32 tex_idx, u32 abgr )
{
    f32 ccx = lx + lw * 0.5f, ccy = ly + lh * 0.5f;
    s_tess.cur_rot_c = cs;
    s_tess.cur_rot_s = sn;
    tess_quad_push( px + ccx * cs - ccy * sn, py + ccx * sn + ccy * cs,
                    lw * 0.5f, lh * 0.5f, GUI_QUAD_RULE_EXACT,
                    0u, 0u, tex_idx, abgr, glyph_id );
}

/* Tessellate a glyph run under a uniform scale and a rotation about its origin (the text_xf
   command).  The run is laid out in its OWN space -- pen at 0, the font's unscaled advances -- and
   the whole of it is mapped once per glyph quad, so the transform never accumulates: 200 glyphs in
   and the pen is still exactly `sum(advance) * scale` from the origin along the rotated axis.

   Nothing about the ATLAS side changes: the same glyph-table IDs, the same tex, the same batch key
   as the 1:1 path, so a rotated run merges into the very same draw call as the upright text beside
   it as long as both are in the same font.  What makes it LOOK right rather than merely be placed
   right is the sampling model -- a coverage font is point-sampled and will show its texels here,
   while a distance-field font resolves its edge in the fragment from a screen-space derivative and
   is therefore indifferent to both the scale and the angle. */
static void
tess_text_xf( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 scale, f32 rot )
{
    u32 tex = font_tex();
    if ( tex == 0 || scale <= 0.0f )
        return;

    s_tess.cur_is_text = true;        /* every quad below is a character (glyph attribution) */
    s_tess.slot_text_runs++;

    f32 cs = cosf( rot ), sn = sinf( rot );
    f32 pen = 0.0f;                      /* run-local, UNSCALED: scale is applied at the map */

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        u32 gid;
        f32 ox, oy, gw, gh, advance;
        font_glyph_placed( cp, &gid, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
            tess_glyph_xf( x, y, cs, sn,
                           ( pen + ox ) * scale, oy * scale, gw * scale, gh * scale,
                           gid, tex, abgr );

        pen += advance;
    }

    s_tess.cur_is_text = false;
}

/* Tessellate a dashed / dotted line as one oriented textured quad sampling the atlas dash row.
   U spans 0..len/period so the row tiles along the line under REPEAT-U addressing; V selects the
   baked row whose on-fraction is closest to `duty`.  O(1) geometry regardless of line length --
   the per-dash quad explosion (which used to exhaust the command list) is gone. */
static void
tess_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 period, f32 duty, u32 abgr )
{
    if ( thickness <= 0.0f || period <= 0.0f )
        return;
    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;
    f32 inv  = 1.0f / len;
    f32 ux   = dx * inv, uy = dy * inv;          /* unit vector along the line  */
    f32 half = thickness * 0.5f;
    f32 umax = len / period;                     /* number of tiled periods -> U span */
    f32 vv   = res_atlas_dash_v( duty );

    /* U runs 0..1 in the quad's uv lanes and is multiplied back up to `umax` periods by the
       fragment: the packed UV cannot hold a coordinate past 1, and the sampler's REPEAT-U is
       what tiles the atlas dash row (gui.h, GUI_OP_TILE_U).  The line's direction is the quad's
       turn -- a style per direction; dashed lines are rare enough that the dedup loss is noise. */
    s_tess.cur_prim.pat_size = umax;
    s_tess.cur_ops |= GUI_OP_TILE_U;
    s_tess.cur_rot_c = ux;
    s_tess.cur_rot_s = uy;
    tess_quad_push( ( x0 + x1 ) * 0.5f, ( y0 + y1 ) * 0.5f, len * 0.5f, half,
                    GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( 0.0f, vv ), gui_uv_pack( 1.0f, vv ),
                    res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}


// clang-format on
