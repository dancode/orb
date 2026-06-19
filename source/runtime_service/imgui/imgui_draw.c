/*==============================================================================================

    runtime_service/imgui/imgui_draw.c -- Draw list accumulation.

    All geometry goes through draw_push_rect_filled or draw_push_triangle.
    draw_ensure_cmd opens a new draw command when the texture or clip rect changes.
    draw_push_text emits glyph quads from the font atlas.

    Included by imgui.c after imgui_font.c so font_glyph / s_atlas_idx are in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    imgui_gpu_cmd_t -- backend-private GPU draw command.

    One bounded range of indices sharing a texture slot and scissor rect -- the unit the GPU
    sees.  Not exposed in imgui.h.  The public imgui_cmd_t carries semantic shapes; the render
    backend (imgui_render.c) tessellates those into these at flush time.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    u32          elem_count; /* number of indices to emit */
    u32          tex_idx;    /* bindless texture slot     */
    imgui_rect_t clip_rect;  /* scissor rect (pixels)     */

} imgui_gpu_cmd_t;

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static struct
{
    imgui_draw_vert_t  verts        [ IMGUI_MAX_VERTS ];
    u16                indices      [ IMGUI_MAX_IDX ];
    imgui_gpu_cmd_t    cmds         [ IMGUI_MAX_CMDS ];
    u32                cmd_z        [ IMGUI_MAX_CMDS ];  // per-command sort key (z), parallel to cmds[]
    u32                cmd_vp       [ IMGUI_MAX_CMDS ];  // per-command target viewport index, parallel to cmds[]

    u32                vert_count;  // verts currently in the list (up to IMGUI_MAX_VERTS)
    u32                idx_count;   // indices currently in the list (up to IMGUI_MAX_IDX)
    u32                cmd_count;   // draw commands currently in the list (up to IMGUI_MAX_CMDS)

    u32                cur_z;       // sort key stamped onto new commands (set by begin/end_window)
    u32                cur_vp;      // viewport index stamped onto new commands (set by begin/end_window)

    imgui_rect_t       clip_stack   [ IMGUI_CLIP_DEPTH ];
    u32                clip_depth;

    /* Global opacity multiplier applied to every pushed quad / triangle (and so to text, which
       routes through the quad path).  1.0 normally; lowered for the span of a disabled item so
       it dims with no per-widget code, then reset by the item / chrome seams.  See draw_set_alpha. */
    f32                alpha;

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
    s_draw.cur_vp     = 0;       /* main viewport; windows route via draw_set_viewport */
    s_draw.clip_depth = 1;
    s_draw.overflow   = false;   /* per-frame; hwm + overflow_ever persist */
    s_draw.alpha      = 1.0f;    /* opaque; lowered per-item for disabled draws */

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
    /* Intersect with the enclosing clip so a nested region (a child box near a window edge)
       can never scissor outside its parent.  The push always happens -- a fully clipped-out
       region pushes a zero-size rect, which simply draws nothing -- so every push still has a
       matching pop and the stack stays balanced. */
    imgui_rect_t c = rect_intersect( ( imgui_rect_t ){ x, y, w, h }, draw_current_clip() );

    if ( s_draw.clip_depth < IMGUI_CLIP_DEPTH )
        s_draw.clip_stack[ s_draw.clip_depth++ ] = c;

    /* Debug overlay: record this clip rect, colored by its new stack depth. */
    DBG_CLIP( c, s_draw.clip_depth );
}

static void
draw_pop_clip_rect( void )
{
    if ( s_draw.clip_depth > 1 )
        --s_draw.clip_depth;
}

/* Set the base clip (clip_stack[0]) -- the rect every window clip intersects against -- to a given
   surface size.  draw_reset seeds it to the main display; begin_window overwrites it with its own
   viewport's drawable size so a window on a second surface is bounded by that surface, not the main
   window (end_window restores the main display).  Only touches slot 0, so it is safe whenever the
   stack is at base depth (between windows); a window's own clip pushes on top of it. */
static void
draw_set_root_clip( f32 w, f32 h )
{
    s_draw.clip_stack[ 0 ] = ( imgui_rect_t ){ 0.0f, 0.0f, w, h };
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

/* Current sort key -- saved by the popup layer so an overlay window can restore the parent's
   paint order on close (begin/end_window drive cur_z, which is a single global). */
static u32
draw_sort_key( void )
{
    return s_draw.cur_z;
}

/*----------------------------------------------------------------------------------------------
    draw_set_viewport -- route subsequent commands to viewport `vp` (the surface a window paints).

    Set to the window's assigned viewport in begin_window and back to 0 (the main swapchain) in
    end_window, exactly as draw_set_sort_key drives the paint order.  flush replays only the
    commands tagged with its own viewport index, so one context can build every window's geometry
    and dispatch each window to the surface hosting it.
----------------------------------------------------------------------------------------------*/

static void
draw_set_viewport( u32 vp )
{
    s_draw.cur_vp = vp;
}

/* Current viewport -- saved/restored by the popup layer alongside the sort key, so an overlay
   begun mid-window leaves the parent's routing intact (begin/end_window drive cur_vp globally). */
static u32
draw_viewport( void )
{
    return s_draw.cur_vp;
}

/* draw_push_clip_root -- push the full-display clip (clip_stack[0]) as a fresh top, WITHOUT
   intersecting the current clip.  A popup is a top-level overlay: it must escape the enclosing
   window's clip, so the popup layer pushes this before opening the popup window (whose own clip
   then intersects the display, not the parent) and pops it after.  Balanced with draw_pop_clip_rect. */
static void
draw_push_clip_root( void )
{
    if ( s_draw.clip_depth < IMGUI_CLIP_DEPTH )
        s_draw.clip_stack[ s_draw.clip_depth++ ] = s_draw.clip_stack[ 0 ];
}

/*----------------------------------------------------------------------------------------------
    Global alpha -- a per-item opacity multiplier folded into every quad / triangle.

    draw_set_alpha installs the multiplier (clamped to [0,1]); draw_apply_alpha scales a packed
    color's A byte by it.  The item-flag resolver lowers it for the span of a disabled widget so
    the whole widget dims with no per-widget code, and the frame / chrome seams reset it to 1.0
    (chrome is not an item, so it always paints opaque).  At 1.0 the byte is returned unchanged.
----------------------------------------------------------------------------------------------*/

static void
draw_set_alpha( f32 a )
{
    s_draw.alpha = a < 0.0f ? 0.0f : ( a > 1.0f ? 1.0f : a );
}

static u32
draw_apply_alpha( u32 abgr )
{
    if ( s_draw.alpha >= 1.0f ) return abgr;                /* opaque -- the common path */
    u32 a = ( abgr >> 24 ) & 0xFFu;
    a = (u32)( (f32)a * s_draw.alpha + 0.5f );              /* scale + round the alpha byte */
    return ( abgr & 0x00FFFFFFu ) | ( a << 24 );
}

/*----------------------------------------------------------------------------------------------
    draw_ensure_cmd -- open a new command when texture, clip, or sort key changes
----------------------------------------------------------------------------------------------*/

static void
draw_ensure_cmd( u32 tex_idx, imgui_rect_t clip )
{
    if ( s_draw.cmd_count > 0 )
    {
        const imgui_gpu_cmd_t* cur = &s_draw.cmds[ s_draw.cmd_count - 1 ];
        /* A sort-key OR viewport change must break the command: flush reorders ranges by z and
           replays each viewport's ranges to a different surface, so neither may merge across the
           boundary -- never merge across windows or across surfaces. */
        if ( s_draw.cmd_z[ s_draw.cmd_count - 1 ] == s_draw.cur_z
             && s_draw.cmd_vp[ s_draw.cmd_count - 1 ] == s_draw.cur_vp
             && cur->tex_idx == tex_idx
             && cur->clip_rect.x == clip.x && cur->clip_rect.y == clip.y
             && cur->clip_rect.w == clip.w && cur->clip_rect.h == clip.h )
            return; /* can append to existing command */
    }

    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;

    s_draw.cmd_z[ s_draw.cmd_count ]  = s_draw.cur_z;
    s_draw.cmd_vp[ s_draw.cmd_count ] = s_draw.cur_vp;
    s_draw.cmds[ s_draw.cmd_count++ ] = ( imgui_gpu_cmd_t ){
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

    abgr = draw_apply_alpha( abgr );   /* fold in the per-item opacity (disabled dim) */

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

    abgr = draw_apply_alpha( abgr );   /* fold in the per-item opacity (disabled dim) */

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
    draw_prim_begin / draw_prim_commit -- raw vertex/index reservation for custom geometry.

    The rect / triangle pushers above each build one fixed shape with a single color.  Stroking a
    line or path needs per-vertex colors (the antialiased edge fades a vertex's alpha to 0) and a
    variable vertex count, so it writes the buffers directly: draw_prim_begin guards overflow,
    reserves nv verts and ni indices, opens the solid-white draw command, and hands back write
    pointers, the base vertex index, and the white-texel UV every vertex must carry.  The caller
    fills [*out_v .. nv) and [*out_i .. ni) then calls draw_prim_commit with the same counts.

    Indices are written relative to *out_base (the absolute first vertex), so a stroke builder adds
    its local 0..nv-1 vertex numbers to that base.  Used by imgui_draw_path.c.
----------------------------------------------------------------------------------------------*/

static bool
draw_prim_begin( u32 nv, u32 ni, f32* wu, f32* wv,
                 imgui_draw_vert_t** out_v, u16** out_i, u16* out_base )
{
    if ( s_draw.vert_count + nv > IMGUI_MAX_VERTS || s_draw.idx_count + ni > IMGUI_MAX_IDX )
    {
        s_draw.overflow = true;
        return false;
    }

    /* Solid geometry rides the font atlas's white texel, same convention as the rect/tri path. */
    u32          tex  = font_atlas_idx();
    imgui_rect_t clip = draw_current_clip();
    draw_ensure_cmd( tex, clip );
    if ( s_draw.cmd_count == 0 )
        return false;

    font_white_uv( wu, wv );
    *out_base = (u16)s_draw.vert_count;
    *out_v    = &s_draw.verts[ s_draw.vert_count ];
    *out_i    = &s_draw.indices[ s_draw.idx_count ];
    return true;
}

static void
draw_prim_commit( u32 nv, u32 ni )
{
    s_draw.vert_count += nv;
    s_draw.idx_count  += ni;
    s_draw.cmds[ s_draw.cmd_count - 1 ].elem_count += ni;
}

/*----------------------------------------------------------------------------------------------
    draw_push_circle_filled -- a solid disc as a triangle fan around its center.

    `segments` facets approximate the circle; ~16 reads as round at widget sizes.  Built on
    draw_push_triangle so it inherits the solid-color white-texel routing, clipping, and overflow
    guard -- the engine keeps "all geometry is rects or triangles" with no new vertex path.  A
    radio button stacks two of these (border ring + inner well) plus a third for the selected dot.
----------------------------------------------------------------------------------------------*/

static void
draw_push_circle_filled( f32 cx, f32 cy, f32 r, u32 segments, u32 abgr )
{
    if ( segments < 3 ) segments = 3;

    f32 step = 6.2831853f / (f32)segments;   /* 2*pi / n */
    f32 px = cx + r, py = cy;                 /* vertex at angle 0 */
    for ( u32 i = 1; i <= segments; ++i )
    {
        f32 a  = step * (f32)i;
        f32 nx = cx + cosf( a ) * r;
        f32 ny = cy + sinf( a ) * r;
        draw_push_triangle( cx, cy, px, py, nx, ny, 0, abgr );
        px = nx; py = ny;
    }
}

/*----------------------------------------------------------------------------------------------
    draw_push_text -- push glyph quads for a NUL-terminated string
----------------------------------------------------------------------------------------------*/

/* Emit at most n bytes of str (stops early at a NUL).  Labels draw only their visible span --
   the bytes before a "##" marker -- through this; draw_push_text is the whole-string case. */
static void
draw_push_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n )
{
    f32 cx = x;
    for ( u32 i = 0; i < n && str[ i ]; ++i )
    {
        u8  ch = (u8)str[ i ];
        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( ch, &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );
        if ( gw > 0.0f && gh > 0.0f )
            draw_push_rect_filled( cx + ox, y + oy, gw, gh, u0, v0, u1, v1, font_atlas_idx(), abgr );
        cx += advance;
    }
}

static void
draw_push_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text_n( x, y, abgr, str, 0xFFFFFFFFu );
}

// clang-format on
/*============================================================================================*/
