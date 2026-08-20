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

void
draw_push_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    /* Rounded outlines become GUI_OP_BAND surfaces with an AA skirt past the authored rect --
       the same 1 px cull slack the rounded fill takes (see draw_rect_cmd). */
    f32 rounding = draw_clamp_rounding( w, h );

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

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_RECT_OUTLINE, col, x, y, w, h, pad );
    if ( !c )
        return;
    c->rect_outline.x        = x;
    c->rect_outline.y        = y;
    c->rect_outline.w        = w;
    c->rect_outline.h        = h;
    c->rect_outline.t        = t;
    c->rect_outline.abgr     = col;
    c->rect_outline.rounding = rounding;
    c->rect_outline.corner_pow = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_frame -- the widget bezel: filled body + border band as ONE command.

    The fragment composites the band (col_border, `t` px inside the boundary) over the fill in a
    single quad (GUI_OP_FRAME), where the fill + outline pair costs two quads rounded and five
    square.  The pair still exists below as the fallback for what one field cannot say.
==============================================================================================*/

void
draw_push_frame( f32 x, f32 y, f32 w, f32 h, f32 t, u32 col_bg, u32 col_border )
{
    u32 fill = draw_apply_alpha( col_bg );
    u32 bord = draw_apply_alpha( col_border );

    /* One side invisible degenerates to the primitive the other side is; an outward-aligned
       band reaches past the boundary the fill's field ends at, so it keeps the pair. */
    if ( t <= 0.0f || ( bord >> 24 ) == 0u )
    {
        draw_push_rect_filled( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0, col_bg );
        return;
    }
    if ( ( fill >> 24 ) == 0u )
    {
        draw_push_rect_outline( x, y, w, h, t, col_border );
        return;
    }
    if ( s_draw.border_align > 0.0f )
    {
        draw_push_rect_filled( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0, col_bg );
        draw_push_rect_outline( x, y, w, h, t, col_border );
        return;
    }

    f32 rounding = draw_clamp_rounding( w, h );
    f32 pad      = ( rounding > 0.0f ) ? 1.0f : 0.0f;

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_FRAME, fill | bord, x, y, w, h, pad );
    if ( !c )
        return;
    c->frame.x          = x;
    c->frame.y          = y;
    c->frame.w          = w;
    c->frame.h          = h;
    c->frame.t          = t;
    c->frame.rounding   = rounding;
    c->frame.corner_pow = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    c->frame.abgr       = fill;
    c->frame.col_border = bord;
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

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_TRIANGLE, col, minx, miny,
                                  maxx - minx, maxy - miny, 0.0f );
    if ( !c )
        return;
    c->tri.ax   = ax; c->tri.ay = ay;
    c->tri.bx   = bx; c->tri.by = by;
    c->tri.cx   = cx; c->tri.cy = cy;
    c->tri.abgr = col;
    draw_cmd_seal();
}

// clang-format on
/*============================================================================================*/
