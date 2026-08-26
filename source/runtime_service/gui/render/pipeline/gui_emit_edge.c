/*==============================================================================================
    gui/render/pipeline/gui_emit_edge.c -- Outlines, bezels and the bare triangle.

    draw_push_rect_outline resolves the ambient border alignment here and nowhere else: an aligned
    band is the INSIDE band of the shape inflated by align * width, so neither the tessellator nor
    the fragment ever learns the concept.  draw_push_frame is the fill and its border band in one
    command (GUI_OP_FRAME), falling back to the pair when the alignment pushes the band outward.

    After gui_emit_shape.c -- the bezel fallback calls draw_push_rect_filled / _outline.
==============================================================================================*/
// clang-format off

/*==============================================================================================
    draw_push_rect_outline -- emit a hollow rectangle semantic command.
==============================================================================================*/

/* Shared base for a stroked rect: `rounding` arrives explicit and already clamped, the same
   convention draw_rect_cmd's callers follow -- the ambient-reading draw_push_rect_outline below is
   the thin wrapper, and draw_push_frame reaches this directly so its own explicit rounding
   survives every one of its fallback branches instead of being silently re-read from the ambient. */
static void
draw_rect_outline_cmd( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr, f32 rounding )
{
    /* Border alignment, resolved here and nowhere else: an aligned band is the INSIDE band of
       the shape inflated by align * width, with the radius growing by the same amount so the
       corners stay concentric with the authored ones.  A square outline (radius 0) keeps its
       sharp corner -- the mitre join -- because the inflation moves edges, not arcs. */
    f32 ba = s_draw.border_align * t;
    if ( ba > 0.0f )
    {
        x -= ba;  y -= ba;  w += ba * 2.0f;  h += ba * 2.0f;
        if ( rounding > 0.0f )
        {
            rounding += ba;
            f32 lim = ( ( w < h ) ? w : h ) * 0.5f;
            if ( rounding > lim ) rounding = lim;
        }
    }
    f32 pad      = ( rounding > 0.0f ) ? 1.0f : 0.0f;
    u32 col      = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_RECT_OUTLINE, col, x, y, w, h, pad );
    if ( !e )
        return;
    e->rect_outline.x        = x;
    e->rect_outline.y        = y;
    e->rect_outline.w        = w;
    e->rect_outline.h        = h;
    e->rect_outline.t        = t;
    e->rect_outline.abgr     = col; /* stored in the quad's color field */
    e->rect_outline.rounding = rounding;
    e->rect_outline.corner_pow = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    draw_cmd_seal();
}

void
draw_push_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    /* Rounded outlines become GUI_OP_BAND surfaces with an AA skirt past the authored rect --
       the same 1 px cull slack the rounded fill takes (see draw_rect_cmd). */
    draw_rect_outline_cmd( x, y, w, h, t, abgr, draw_clamp_rounding( w, h ) );
}

/*==============================================================================================
    draw_push_frame -- the widget bezel: filled body + border band as ONE command.

    The fragment composites the band (col_border, `t` px inside the boundary) over the fill in a
    single quad (GUI_OP_FRAME), where the fill + outline pair costs two quads rounded and five
    square.  The pair still exists below as the fallback for what one field cannot say.

    `rounding` arrives explicit -- like draw_push_round_rect_ex, NOT read from the ambient here.
    Every fallback branch below reaches draw_rect_cmd / draw_rect_outline_cmd directly instead of
    the ambient-reading draw_push_rect_filled / draw_push_rect_outline, so the value passed in is
    the value every branch actually draws with, not just the common one.
==============================================================================================*/

void
draw_push_frame( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 t, u32 col_bg, u32 col_border )
{
    u32 fill = draw_apply_alpha( col_bg );
    u32 bord = draw_apply_alpha( col_border );

    rounding = draw_clamp_rounding_val( rounding, w, h );

    /* One side invisible degenerates to the primitive the other side is; an outward-aligned
       band reaches past the boundary the fill's field ends at, so it keeps the pair. */
    if ( t <= 0.0f || ( bord >> 24 ) == 0u )
    {
        draw_rect_cmd( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0, col_bg, rounding, s_draw.corner_pow );
        return;
    }
    if ( ( fill >> 24 ) == 0u )
    {
        draw_rect_outline_cmd( x, y, w, h, t, col_border, rounding );
        return;
    }
    if ( s_draw.border_align > 0.0f )
    {
        draw_rect_cmd( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0, col_bg, rounding, s_draw.corner_pow );
        draw_rect_outline_cmd( x, y, w, h, t, col_border, rounding );
        return;
    }

    f32 pad = ( rounding > 0.0f ) ? 1.0f : 0.0f;

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_FRAME, fill | bord, x, y, w, h, pad );
    if ( !e )
        return;

    e->frame.x          = x;
    e->frame.y          = y;
    e->frame.w          = w;
    e->frame.h          = h;
    e->frame.t          = t;
    e->frame.rounding   = rounding;
    e->frame.corner_pow = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    e->frame.abgr       = fill; /* stored in the tess_fx_box color body field */
    e->frame.col_border = bord; /* stored in the quad's color field */
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_round_frame_ex -- draw_push_frame's per-corner sibling.

    Every fallback below reaches draw_push_round_rect_ex directly (fill via border 0, outline via
    border t) rather than draw_rect_cmd / draw_rect_outline_cmd, which only know one shared
    rounding -- the same "explicit radii survive every branch" rule draw_push_frame follows for
    its own single radius.
==============================================================================================*/

void
draw_push_round_frame_ex( f32 x, f32 y, f32 w, f32 h,
                         f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 t,
                         u32 col_bg, u32 col_border )
{
    u32 fill = draw_apply_alpha( col_bg );
    u32 bord = draw_apply_alpha( col_border );

    /* One side invisible degenerates to the primitive the other side is; an outward-aligned band
       reaches past the boundary the fill's field ends at, so it keeps the pair -- draw_push_round_
       rect_ex's stroke has no align concept of its own, same as the uniform path without one. */
    if ( t <= 0.0f || ( bord >> 24 ) == 0u )
    {
        draw_push_round_rect_ex( x, y, w, h, rtl, rtr, rbr, rbl, 0.0f, 0.0f,
                                 col_bg, col_bg, 0.0f, (u32)GUI_GRAD_LINEAR, 0.0f );
        return;
    }
    if ( ( fill >> 24 ) == 0u )
    {
        draw_push_round_rect_ex( x, y, w, h, rtl, rtr, rbr, rbl, 0.0f, t,
                                 col_border, col_border, 0.0f, (u32)GUI_GRAD_LINEAR, 0.0f );
        return;
    }
    if ( s_draw.border_align > 0.0f )
    {
        draw_push_round_rect_ex( x, y, w, h, rtl, rtr, rbr, rbl, 0.0f, 0.0f,
                                 col_bg, col_bg, 0.0f, (u32)GUI_GRAD_LINEAR, 0.0f );
        draw_push_round_rect_ex( x, y, w, h, rtl, rtr, rbr, rbl, 0.0f, t,
                                 col_border, col_border, 0.0f, (u32)GUI_GRAD_LINEAR, 0.0f );
        return;
    }

    f32 rmax = rtl;
    if ( rtr > rmax ) rmax = rtr;
    if ( rbr > rmax ) rmax = rbr;
    if ( rbl > rmax ) rmax = rbl;
    f32 pad = ( rmax > 0.0f ) ? 1.0f : 0.0f;

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_ROUND_FRAME_EX, fill | bord, x, y, w, h, pad );
    if ( !e )
        return;

    e->round_frame_ex.x          = x;
    e->round_frame_ex.y          = y;
    e->round_frame_ex.w          = w;
    e->round_frame_ex.h          = h;
    e->round_frame_ex.t          = t;
    e->round_frame_ex.rtl        = rtl;
    e->round_frame_ex.rtr        = rtr;
    e->round_frame_ex.rbr        = rbr;
    e->round_frame_ex.rbl        = rbl;
    e->round_frame_ex.corner_pow = ( rmax > 0.0f ) ? s_draw.corner_pow : 0.0f;
    e->round_frame_ex.abgr       = fill;
    e->round_frame_ex.col_border = bord;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_triangle -- emit a solid triangle semantic command.
==============================================================================================*/

void
draw_push_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    u32 col = draw_apply_alpha( abgr );

    /* Cull against the bounding box of the three vertices. */
    f32 minx = ax < bx ? ( ax < cx ? ax : cx ) : ( bx < cx ? bx : cx );
    f32 maxx = ax > bx ? ( ax > cx ? ax : cx ) : ( bx > cx ? bx : cx );
    f32 miny = ay < by ? ( ay < cy ? ay : cy ) : ( by < cy ? by : cy );
    f32 maxy = ay > by ? ( ay > cy ? ay : cy ) : ( by > cy ? by : cy );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_TRIANGLE, col, minx, miny,
                                          maxx - minx, maxy - miny, 0.0f );
    if ( !e )
        return;
    e->tri.ax   = ax; e->tri.ay = ay;
    e->tri.bx   = bx; e->tri.by = by;
    e->tri.cx   = cx; e->tri.cy = cy;
    e->tri.abgr = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_bezier -- emit a stroked quadratic bezier semantic command.
==============================================================================================*/

void
draw_push_bezier( f32 ax, f32 ay, f32 cx, f32 cy, f32 bx, f32 by, f32 thickness, u32 abgr )
{
    u32 col  = draw_apply_alpha( abgr );
    f32 half = thickness * 0.5f + 1.0f;   /* half-thickness plus a pixel of AA skirt slack */

    /* Cull against the bounding box of the three control points -- a conservative but cheap
       cover for the curve itself, which never leaves their convex hull. */
    f32 minx = ax < bx ? ( ax < cx ? ax : cx ) : ( bx < cx ? bx : cx );
    f32 maxx = ax > bx ? ( ax > cx ? ax : cx ) : ( bx > cx ? bx : cx );
    f32 miny = ay < by ? ( ay < cy ? ay : cy ) : ( by < cy ? by : cy );
    f32 maxy = ay > by ? ( ay > cy ? ay : cy ) : ( by > cy ? by : cy );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_BEZIER, col, minx - half, miny - half,
                                          maxx - minx + 2.0f * half, maxy - miny + 2.0f * half, 0.0f );
    if ( !e )
        return;
    e->bezier.ax        = ax; e->bezier.ay = ay;
    e->bezier.cx        = cx; e->bezier.cy = cy;
    e->bezier.bx        = bx; e->bezier.by = by;
    e->bezier.thickness = thickness;
    e->bezier.abgr      = col;
    draw_cmd_seal();
}

// clang-format on
/*============================================================================================*/
