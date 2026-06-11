/*==============================================================================================

    runtime_service/imgui/imgui_draw.c -- Draw list accumulation.

    All geometry goes through draw_push_rect_filled or draw_push_triangle.
    draw_ensure_cmd opens a new draw command when the texture or clip rect changes.
    draw_push_text emits glyph quads from the font atlas.

    Included by imgui.c after imgui_font.c so font_glyph / s_atlas_idx are in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static struct
{
    imgui_draw_vert_t  verts        [ IMGUI_MAX_VERTS ];
    u16                indices      [ IMGUI_MAX_IDX ];
    imgui_draw_cmd_t   cmds         [ IMGUI_MAX_CMDS ];
    u32                cmd_z        [ IMGUI_MAX_CMDS ];  // per-command sort key (z), parallel to cmds[]

    u32                vert_count;  // verts currently in the list (up to IMGUI_MAX_VERTS)
    u32                idx_count;   // indices currently in the list (up to IMGUI_MAX_IDX)
    u32                cmd_count;   // draw commands currently in the list (up to IMGUI_MAX_CMDS)

    u32                cur_z;       // sort key stamped onto new commands (set by begin/end_window)

    imgui_rect_t       clip_stack   [ IMGUI_CLIP_DEPTH ];
    u32                clip_depth;

    /* Usage tracking (lifetime; draw_reset clears only the per-frame overflow flag). */

    u32                vert_hwm;       // peak vert_count seen across all frames
    u32                idx_hwm;        // peak idx_count  seen across all frames
    bool               overflow;       // a push was dropped this frame (cleared each frame)
    bool               overflow_ever;  // overflow happened at least once this run

} s_draw;

/*----------------------------------------------------------------------------------------------
    draw_reset -- call at the top of new_frame
----------------------------------------------------------------------------------------------*/

static void
draw_reset( i32 display_w, i32 display_h )
{
    s_draw.vert_count = 0;
    s_draw.idx_count  = 0;
    s_draw.cmd_count  = 0;
    s_draw.cur_z      = 0;       /* background; windows raise it via draw_set_sort_key */
    s_draw.clip_depth = 1;
    s_draw.overflow   = false;   /* per-frame; hwm + overflow_ever persist */

    /* first is a default "no clip" rect covering the whole display; never popped off the stack. */
    s_draw.clip_stack[ 0 ] = ( imgui_rect_t ){ 0.0f, 0.0f, (f32)display_w, (f32)display_h };
}

/*----------------------------------------------------------------------------------------------
    Clip stack
----------------------------------------------------------------------------------------------*/

static imgui_rect_t
draw_current_clip( void )
{
    return s_draw.clip_stack[ s_draw.clip_depth - 1 ];
}

static void
draw_push_clip_rect( f32 x, f32 y, f32 w, f32 h )
{
    if ( s_draw.clip_depth < IMGUI_CLIP_DEPTH )
        s_draw.clip_stack[ s_draw.clip_depth++ ] = ( imgui_rect_t ){ x, y, w, h };
}

static void
draw_pop_clip_rect( void )
{
    if ( s_draw.clip_depth > 1 )
        --s_draw.clip_depth;
}

/*----------------------------------------------------------------------------------------------
    draw_set_sort_key -- stamp subsequent commands with this z (window paint order).
    Set to the window's z in begin_window and back to 0 (background) in end_window.
----------------------------------------------------------------------------------------------*/

static void
draw_set_sort_key( u32 z )
{
    s_draw.cur_z = z;
}

/*----------------------------------------------------------------------------------------------
    draw_ensure_cmd -- open a new command when texture, clip, or sort key changes
----------------------------------------------------------------------------------------------*/

static void
draw_ensure_cmd( u32 tex_idx, imgui_rect_t clip )
{
    if ( s_draw.cmd_count > 0 )
    {
        const imgui_draw_cmd_t* cur = &s_draw.cmds[ s_draw.cmd_count - 1 ];
        /* A sort-key change must break the command so flush can reorder the two
           ranges independently -- never merge across windows. */
        if ( s_draw.cmd_z[ s_draw.cmd_count - 1 ] == s_draw.cur_z
             && cur->tex_idx == tex_idx
             && cur->clip_rect.x == clip.x && cur->clip_rect.y == clip.y
             && cur->clip_rect.w == clip.w && cur->clip_rect.h == clip.h )
            return; /* can append to existing command */
    }

    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;

    s_draw.cmd_z[ s_draw.cmd_count ] = s_draw.cur_z;
    s_draw.cmds[ s_draw.cmd_count++ ] = ( imgui_draw_cmd_t ){
        .elem_count = 0,
        .tex_idx    = tex_idx,
        .clip_rect  = clip,
    };
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_filled -- push a textured / solid quad (6 indices, 4 vertices)

    For solid-color draws pass the white-pixel tex_idx with u0=v0=0, u1=v1=1.
    For glyph draws pass the atlas tex_idx with the glyph's UV rect.
----------------------------------------------------------------------------------------------*/

static void
draw_push_rect_filled( f32 x, f32 y, f32 w, f32 h,
                       f32 u0, f32 v0, f32 u1, f32 v1,
                       u32 tex_idx, u32 abgr )
{
    /* Drop the quad if it would exceed either buffer; flag so flush can warn once. */
    if ( s_draw.vert_count + 4 > IMGUI_MAX_VERTS || s_draw.idx_count + 6 > IMGUI_MAX_IDX )
    {
        s_draw.overflow = true;
        return;
    }

    /* tex_idx 0 is the solid-color convention: point at the font atlas's white texel so
       solid fills carry the same texture as text and merge into one draw command. */
    if ( tex_idx == 0 )
    {
        tex_idx = font_atlas_idx();
        font_white_uv( &u0, &v0 );
        u1 = u0;
        v1 = v0;
    }

    /* Pixel-grid snap: round the quad origin to the nearest integer pixel.  The
       ortho maps integer coords exactly onto pixel boundaries, so a snapped origin
       keeps thin edges (1px borders, slider/checkbox outlines, the text cursor) and
       glyph quads crisp instead of straddling two pixels and blurring or vanishing.
       Width/height are left intact -- with integer extents the far edge stays on the
       grid too; fractional extents (e.g. a slider fill bar) keep their exact length. */
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );

    imgui_rect_t clip = draw_current_clip();
    draw_ensure_cmd( tex_idx, clip );
    if ( s_draw.cmd_count == 0 )
         return;

    u16 base = (u16)s_draw.vert_count;

    imgui_draw_vert_t* v = &s_draw.verts[ s_draw.vert_count ];
    v[ 0 ] = ( imgui_draw_vert_t ){ x,     y,     u0, v0, abgr };    /* top-left     */
    v[ 1 ] = ( imgui_draw_vert_t ){ x + w, y,     u1, v0, abgr };    /* top-right    */
    v[ 2 ] = ( imgui_draw_vert_t ){ x + w, y + h, u1, v1, abgr };    /* bottom-right */
    v[ 3 ] = ( imgui_draw_vert_t ){ x,     y + h, u0, v1, abgr };    /* bottom-left  */
    s_draw.vert_count += 4;

    u16* idx = &s_draw.indices[ s_draw.idx_count ];
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base + 0; idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
    s_draw.idx_count += 6;

    s_draw.cmds[ s_draw.cmd_count - 1 ].elem_count += 6;
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_outline -- hollow rectangle as four edge quads
----------------------------------------------------------------------------------------------*/

static void
draw_push_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 tex_idx, u32 abgr )
{
    /* this math prevents corners from double blending on alpha < 1 */

    /* top    */ draw_push_rect_filled( x,         y,         w, t,     0,0,1,1, tex_idx, abgr );
    /* bottom */ draw_push_rect_filled( x,         y + h - t, w, t,     0,0,1,1, tex_idx, abgr );
    /* left   */ draw_push_rect_filled( x,         y + t,     t, h-2*t, 0,0,1,1, tex_idx, abgr );
    /* right  */ draw_push_rect_filled( x + w - t, y + t,     t, h-2*t, 0,0,1,1, tex_idx, abgr );
}

/*----------------------------------------------------------------------------------------------
    draw_push_triangle -- push a solid triangle (3 vertices, 3 indices)
----------------------------------------------------------------------------------------------*/

static void
draw_push_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 tex_idx, u32 abgr )
{
    /* Drop the triangle if it would exceed either buffer; flag so flush can warn once. */
    if ( s_draw.vert_count + 3 > IMGUI_MAX_VERTS || s_draw.idx_count + 3 > IMGUI_MAX_IDX )
    {
        s_draw.overflow = true;
        return;
    }

    /* tex_idx 0 is the solid-color convention: route all three verts to the font
       atlas's white texel so the triangle merges with surrounding solid/text draws. */
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 0.0f, u2 = 0.5f, v2 = 1.0f;
    if ( tex_idx == 0 )
    {
        tex_idx = font_atlas_idx();
        f32 wu, wv;
        font_white_uv( &wu, &wv );
        u0 = u1 = u2 = wu;
        v0 = v1 = v2 = wv;
    }

    imgui_rect_t clip = draw_current_clip();
    draw_ensure_cmd( tex_idx, clip );
    if ( s_draw.cmd_count == 0 ) return;

    u16 base = (u16)s_draw.vert_count;

    s_draw.verts[ s_draw.vert_count++ ] = ( imgui_draw_vert_t ){ ax, ay, u0, v0, abgr };
    s_draw.verts[ s_draw.vert_count++ ] = ( imgui_draw_vert_t ){ bx, by, u1, v1, abgr };
    s_draw.verts[ s_draw.vert_count++ ] = ( imgui_draw_vert_t ){ cx, cy, u2, v2, abgr };

    s_draw.indices[ s_draw.idx_count++ ] = base + 0;
    s_draw.indices[ s_draw.idx_count++ ] = base + 1;
    s_draw.indices[ s_draw.idx_count++ ] = base + 2;

    s_draw.cmds[ s_draw.cmd_count - 1 ].elem_count += 3;
}

/*----------------------------------------------------------------------------------------------
    draw_push_text -- push glyph quads for a NUL-terminated string
----------------------------------------------------------------------------------------------*/

static void
draw_push_text( f32 x, f32 y, u32 abgr, const char* str )
{
    f32 cx = x;
    for ( ; *str; ++str )
    {
        u8  ch = (u8)*str;
        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( ch, &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );
        if ( gw > 0.0f && gh > 0.0f )
            draw_push_rect_filled( cx + ox, y + oy, gw, gh, u0, v0, u1, v1, font_atlas_idx(), abgr );
        cx += advance;
    }
}

// clang-format on
/*============================================================================================*/
