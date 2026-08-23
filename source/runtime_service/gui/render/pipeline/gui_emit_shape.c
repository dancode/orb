/*==============================================================================================

    gui/render/pipeline/gui_emit_shape.c -- The quad family: fills, pictures and gradients.

    Every push whose shape is a plain rectangle carrying colour or a texture -- the fill, the
    image, the disc, the batched rect list, the icon, the rotated quad, the sprite, the
    two-colour ramp.

    draw_rect_cmd is the shared body under the first several and lives here because the
    rounded / outlined / bezel variants all resolve back onto it.

==============================================================================================*/

// clang-format off

/*==============================================================================================

    draw_rect_cmd -- shared base function for rect commands

    `rounding` arrives explicit and already resolved -- the wrappers below fold the ambient
    radius in (or not: see the roundable rule on each), and the disc passes its own.

    `corner_pow` travels the same way and for the same reason: a disc's corner IS the shape,
    so the one caller that names its own radius names its own profile too, and gets 
    the circle.

==============================================================================================*/

static void
draw_rect_cmd( f32 x,  f32 y,  f32 w,  f32 h,
               f32 u0, f32 v0, f32 u1, f32 v1,
               u32 tex_idx, u32 abgr, f32 rounding, f32 corner_pow )
{
    /* A rounded quad becomes an SDF surface whose AA skirt reaches past the authored rect
       (tess_fx_box), so cull it with one pixel of slack -- a shape flush against the clip 
       edge keeps its feathered edge. Square quads cull tight. */

    f32 pad = ( rounding > 0.0f ) ? 1.0f : 0.0f;
    u32 col = draw_apply_alpha( abgr );

    /* tex_idx == 0 is the solid-fill case -- the hot RECT_FILL path, no uv/tex_idx to carry.
       Anything else is a texture, which only the cold RECT_TEX path needs to hold. */
    if ( tex_idx == 0u )
    {
        gui_cmd_ext_t* c = draw_cmd_open( GUI_CMD_RECT_FILL, col, x, y, w, h, pad );
        if ( !c )
            return;

        c->rect_fill.x          = x;
        c->rect_fill.y          = y;
        c->rect_fill.w          = w;
        c->rect_fill.h          = h;
        c->rect_fill.abgr       = col;
        c->rect_fill.rounding   = rounding;
        c->rect_fill.corner_pow = ( rounding > 0.0f ) ? corner_pow : 0.0f;

        draw_cmd_seal();
        return;
    }

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_RECT_TEX, col, x, y, w, h, pad );
    if ( !e )
        return;

    e->rect_tex.x           = x;
    e->rect_tex.y           = y;
    e->rect_tex.w           = w;
    e->rect_tex.h           = h;
    e->rect_tex.u0          = u0;
    e->rect_tex.v0          = v0;
    e->rect_tex.u1          = u1;
    e->rect_tex.v1          = v1;
    e->rect_tex.tex_idx     = tex_idx;
    e->rect_tex.abgr        = col;
    e->rect_tex.rounding    = rounding;
    e->rect_tex.corner_pow  = ( rounding > 0.0f ) ? corner_pow : 0.0f;

    draw_cmd_seal();
}

/*==============================================================================================

    draw_push_rect_filled / draw_push_image -- emit a filled / textured quad command.

    Same quad under the hood. The only difference is whether the ambient rounding radius
    (draw_set_rounding) gets applied: draw_push_rect_filled ignores it for icons, draw_push_image
    applies it for pictures. Which one to call depends on what the quad IS, not what it carries --
    see the comment on each below.

    tex_idx == 0 means "solid color": tessellation rewrites it to the atlas's real texture index
    (a valid slot is always required) and sets GUI_OP_SELF, which tells the fragment shader to
    skip sampling entirely and use the color directly -- it never reads a texel at all.
    Pixel-grid snapping happens later, at flush time in the tessellation pass.

    draw_push_rect_filled:
    The general quad: solid fills, and icons (draw_push_icon routes here too). Ambient rounding
    is skipped, because an icon is a symbol, not a frame -- rounding it would silently cut the
    corners off the glyph if it happened to be drawn inside a draw_set_rounding scope.

    A caller that is actually drawing a picture (not a symbol) should call draw_push_image below
    instead, so it gets rounded corners.

    (Note: the tessellator itself has no trouble rounding a textured quad -- tess_fx_box clamps
    UVs across the falloff skirt so a rounded corner never bleeds into a neighbouring atlas
    texel. The split here is purely about caller intent, not a rendering limitation.)

==============================================================================================*/

void
draw_push_rect_filled( f32 x, f32 y, f32 w, f32 h,      // rect
                       f32 u0, f32 v0, f32 u1, f32 v1,  // uv
                       u32 tex_idx, u32 abgr )          // texture slot + color
{
    /* no rounding if a texture */
    draw_rect_cmd( x, y, w, h, u0, v0, u1, v1, tex_idx, abgr,
                   ( tex_idx == 0 ) ? draw_clamp_rounding( w, h ) : 0.0f, s_draw.corner_pow );
}

/*  An IMAGE: an arbitrary texture the caller is showing as a picture (a scene render 
    target, a loaded photo). Identical to draw_push_rect_filled above except the ambient 
    rounding radius is applied, so pictures get rounded corners.

    (Note: the corner isn't a mask cut over the texture -- the fragment shader resolves the
    rounded boundary from the same signed-distance field a rounded solid fill uses, and 
    samples the texture underneath it; see the effect band in gui.h.) */

void
draw_push_image( f32 x,  f32 y,  f32 w,  f32 h,
                 f32 u0, f32 v0, f32 u1, f32 v1,
                 u32 tex_idx, u32 abgr )
{
    draw_rect_cmd( x, y, w, h, u0, v0, u1, v1, tex_idx, abgr,
                   draw_clamp_rounding( w, h ), s_draw.corner_pow );
}

/*==============================================================================================

    draw_push_circle_filled -- Is a rounded rect whose radius reached the half-extent.  
    Not a command type of its own: the tessellator already derives everything a disc needs
    from that shape (the SDF boundary, and the no-snap rule -- a square box whose radius 
    reached its half-extent has no straight edge to keep crisp, and quantizing a moving
    dot's centre is exactly what must not happen).  
    
    The radius is passed EXPLICIT, bypassing the ambient rounding -- a disc is fully round
    by definition, not by scope.

==============================================================================================*/

void
draw_push_circle_filled( f32 cx, f32 cy, f32 r, u32 abgr )
{
    /* x, y, w, h are the bounding box of the disc */ 
    /* u0..v1 are the full atlas UVs (the fragment never samples them) */
    /* tex_idx is 0 for solid color, abgr is the disc's color, */
    /* rounding is r (the radius), corner_pow is 0 (the disc's corner is a circle). */

    draw_rect_cmd( cx - r, cy - r, r * 2.0f, r * 2.0f, 
                   0.0f, 0.0f, 1.0f, 1.0f, 0, abgr, r, 0.0f );
}

/*==============================================================================================

    draw_push_rect_list -- emit N solid rects as ONE semantic command.

    The dense-shape escape valve: a caller drawing hundreds of small fills (timeline bars, graph
    columns, heatmap cells) would otherwise spend one command slot per rect and exhaust
    GUI_MAX_CMDS long before the vertex budget.
    
    Entries are copied into the per-frame rect pool (the CMD_POLYLINE point-pool pattern) and
    tessellated into one quad each at flush time. Per-entry alpha fold + clip cull happens 
    here so the pool holds only visible work.  Entries share the current clip; 
    always square (no rounding), solid color (tex 0, self-sampled).

==============================================================================================*/

void
draw_push_rect_list( const gui_rect_col_t* rects, u32 count )
{
    if ( !rects || draw_emit_blocked( k_cmd_hash_len[ GUI_CMD_RECT_LIST ] ) )
        return;

    u32 offset = s_draw.rect_count;
    for ( u32 i = 0; i < count && s_draw.rect_count < GUI_MAX_RECT_ENTRIES; ++i )
    {
        u32 col = draw_apply_alpha( rects[ i ].abgr );
        if ( ( col >> 24 ) == 0u )   /* invisible under alpha blending (draw_push_rect_filled rule) */
            continue;

        if ( draw_cull_box( rects[ i ].x, rects[ i ].y, rects[ i ].w, rects[ i ].h ) )
            continue;

        s_draw.rect_pool[ s_draw.rect_count ] = rects[ i ];
        s_draw.rect_pool[ s_draw.rect_count ].abgr = col;
        s_draw.rect_count++;
    }
    if ( s_draw.rect_count == offset )
        return;   /* everything culled: no command slot spent */

    gui_cmd_t*     c    = draw_cmd_claim( GUI_CMD_RECT_LIST );
    gui_cmd_ext_t* e    = draw_cmd_claim_ext( c );
    e->rect_list.offset = offset;
    e->rect_list.count  = s_draw.rect_count - offset;

    draw_cmd_seal();   /* entries are L1-hot here */
}

/*==============================================================================================
    draw_push_icon -- push one registered icon quad into the draw list.

    An icon is just a textured quad sourced from the icon atlas instead of the font atlas, so
    this reuses draw_push_rect_filled wholesale; icon_get (the sprite source contract, supplied
    by the draw unit) hands back the cached UVs.  No-op for an invalid id.

    The texture comes from icon_tex( id ) rather than res_atlas_idx(), which is the entire
    draw-side cost of icons being able to be distance fields: an icon names its own atlas AND
    its own sampling model, and one draw call still holds a coverage icon, an SDF icon, 
    a glyph run and a fill.
==============================================================================================*/

void
draw_push_icon( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, u32 abgr )
{
    f32 u0, v0, u1, v1;
    if ( !icon_get( id, &u0, &v0, &u1, &v1, NULL, NULL ) )
        return;

    u32 tex = icon_tex( id );

    if ( tex == 0u )
         return;   /* SDF atlas not stood up yet -- skip the quad, as a glyph run does */

    draw_push_rect_filled( x, y, w, h, u0, v0, u1, v1, tex, abgr );
}

/*==============================================================================================
    draw_push_image_xf / draw_push_icon_xf -- one textured quad turned about its centre.

    The text_xf treatment applied to a single quad: four positions rotate, the UVs interpolate
    exactly as they would upright, and what makes the result legible at any angle is the
    sampling model riding the tex word -- an SDF icon resolves its edge from the screen-space
    derivative and turns cleanly, a coverage icon shows its texels (the same rule the two 
    font bakes follow). Compass needles, minimap markers, spinner glyphs.
==============================================================================================*/

void
draw_push_image_xf( f32 x, f32 y, f32 w, f32 h,
                    f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, f32 rot, u32 abgr )
{
    /* Rotated-AABB cull, the draw_push_box_xf rule; the quad has no skirt, so 1 px of slack. */
    f32 cs = cosf( rot ), sn = sinf( rot );
    f32 hx = w * 0.5f, hy = h * 0.5f;
    f32 ex = fabsf( hx * cs ) + fabsf( hy * sn );
    f32 ey = fabsf( hx * sn ) + fabsf( hy * cs );
    u32 col = draw_apply_alpha( abgr );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_IMAGE_XF, col,
                                          x + hx - ex, y + hy - ey, ex * 2.0f, ey * 2.0f, 1.0f );
    if ( !e )
        return;
    e->image_xf.x       = x;
    e->image_xf.y       = y;
    e->image_xf.w       = w;
    e->image_xf.h       = h;
    e->image_xf.u0      = u0;
    e->image_xf.v0      = v0;
    e->image_xf.u1      = u1;
    e->image_xf.v1      = v1;
    e->image_xf.rot     = rot;
    e->image_xf.tex_idx = tex_idx;
    e->image_xf.abgr    = col;
    draw_cmd_seal();
}

void
draw_push_icon_xf( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, f32 rot, u32 abgr )
{
    f32 u0, v0, u1, v1;
    if ( !icon_get( id, &u0, &v0, &u1, &v1, NULL, NULL ) )
        return;
    u32 tex = icon_tex( id );
    if ( tex == 0u )
        return;   /* SDF atlas not stood up yet -- skip the quad, as draw_push_icon does */
    draw_push_image_xf( x, y, w, h, u0, v0, u1, v1, tex, rot, abgr );
}

/*==============================================================================================
    draw_push_sprite -- emit one sprite quad (optionally nine-sliced) as ONE semantic command.

    The command carries the sprite ID, not its UVs.  An icon resolves at emit because a quad is all
    it will ever be; a sprite must not, for two reasons that both point the same way: the slice
    expansion needs the source's pixel size and insets (which only the registry has), and a
    sprite-atlas repack moves UVs under a command that may live in the retained cache for many
    frames.  Resolving in the tessellator puts both facts in one place and lets the ordinary
    generation-fold re-tessellate path correct a repack with no re-emit.

    One command however many quads it becomes, which is the point: a nine-slice frame costs one
    command slot and one batch, so a window can afford an authored border on every panel.
==============================================================================================*/

void
draw_push_sprite( f32 x, f32 y, f32 w, f32 h, gui_sprite_id_t id,
                  u32 abgr, f32 scale, u16 flags, bool nine )
{
    if ( id == GUI_SPRITE_NONE )
        return;

    /* A tint of 0 means UNTINTED -- the sprite's own colours at full alpha (the gui_brush_t rule).
       Only an explicit tint can fade a sprite, and one faded to zero alpha is invisible under
       blending, so it drops exactly as a transparent fill does. */
    u32 col = draw_apply_alpha( abgr ? abgr : 0xFFFFFFFFu );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_SPRITE, col, x, y, w, h, 0.0f );
    if ( !e )
        return;
    e->sprite.x      = x;
    e->sprite.y      = y;
    e->sprite.w      = w;
    e->sprite.h      = h;
    e->sprite.scale  = ( scale > 0.0f ) ? scale : 1.0f;
    e->sprite.sprite = id;
    e->sprite.abgr   = col;
    e->sprite.flags  = flags;
    e->sprite.nine   = nine ? 1u : 0u;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_rect_gradient -- emit a two-color gradient rectangle as one semantic command.

    col_a / col_b sit on opposite edges (horizontal = left->right, else top->bottom); the fragment
    ramps between them off the prim record (GUI_OP_GRAD), so the fill is one quad at any size.
    Always square (no rounding) -- draw_push_round_rect_gradient is the general form.
==============================================================================================*/

void
draw_push_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    /* Visible if EITHER end is: the OR'd alpha is the visibility word draw_cmd_open tests. */
    u32 ca = draw_apply_alpha( col_a );
    u32 cb = draw_apply_alpha( col_b );

    gui_cmd_ext_t* e = draw_cmd_open( GUI_CMD_RECT_GRADIENT, ca | cb, x, y, w, h, 0.0f );
    if ( !e )
        return;
    e->gradient.x          = x;
    e->gradient.y          = y;
    e->gradient.w          = w;
    e->gradient.h          = h;
    e->gradient.col_a      = ca;
    e->gradient.col_b      = cb;
    e->gradient.horizontal = horizontal;
    draw_cmd_seal();
}

// clang-format on
/*============================================================================================*/
