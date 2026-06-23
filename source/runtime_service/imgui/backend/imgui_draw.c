/*==============================================================================================

    runtime_service/imgui/imgui_draw.c -- Draw list accumulation.

    All geometry goes through draw_push_rect_filled or draw_push_triangle.
    draw_ensure_cmd opens a new draw command when the texture or clip rect changes.
    draw_push_text emits glyph quads from the font atlas.

    Included by imgui_backend.c after imgui_font.c so font_glyph / s_atlas_idx are in scope.

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
    imgui_cmd_t  cmds  [ IMGUI_MAX_CMDS   ];   /* semantic command list; one entry per shape      */
    imgui_vec2_t points[ IMGUI_MAX_PATH_PTS ];  /* point pool for CMD_POLYLINE data; indexed by pt_offset */

    /* Flat string pool: draw_push_text_n copies every string here so that stack-local buffers
       (textf, snprintf labels) remain valid until imgui_render_flush consumes them. */
    char text_pool[ IMGUI_MAX_TEXT_POOL ];
    u32  text_pool_used;

    u32 cmd_count;   /* commands in the list this frame  */
    u32 pt_count;    /* points in the pool this frame    */

    u32 cur_z;       /* sort key stamped onto new commands (set by begin/end_window)        */
    u32 cur_vp;      /* viewport index stamped onto new commands (set by begin/end_window)  */

    imgui_rect_t clip_stack[ IMGUI_CLIP_DEPTH ];
    u32          clip_depth;

    /* Global opacity multiplier applied to every pushed shape.  1.0 normally; lowered for the
       span of a disabled item so it dims with no per-widget code; reset by item / chrome seams. */
    f32 alpha;

    /* Ambient corner radius folded into every filled / outlined rect.  Set from the resolved
       rounding category (window / widget / grab) at the item / chrome seams and at the few sites
       that draw a different category; 0 emits square shapes (the fast path). */
    f32 rounding;

} s_draw;

/*----------------------------------------------------------------------------------------------
    draw_reset -- call at the top of new_frame
----------------------------------------------------------------------------------------------*/

void
draw_reset( i32 display_w, i32 display_h )
{
    s_draw.cmd_count       = 0;
    s_draw.pt_count        = 0;
    s_draw.text_pool_used  = 0;
    s_draw.cur_z           = 0;   /* background; windows raise it via draw_set_sort_key */
    s_draw.cur_vp          = 0;   /* main viewport; windows route via draw_set_viewport */
    s_draw.clip_depth      = 1;
    s_draw.alpha           = 1.0f;
    s_draw.rounding        = 0.0f;   /* square until a seam sets the resolved radius */

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

void
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

void
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
void
draw_set_root_clip( f32 w, f32 h )
{
    s_draw.clip_stack[ 0 ] = ( imgui_rect_t ){ 0.0f, 0.0f, w, h };
}

/*----------------------------------------------------------------------------------------------
    draw_set_sort_key -- stamp subsequent commands with this z (window paint order).
    Set to the window's z in begin_window and back to 0 (background) in end_window.
----------------------------------------------------------------------------------------------*/

void
draw_set_sort_key( u32 z )
{
    s_draw.cur_z = z;
}

/* Current sort key -- saved by the popup layer so an overlay window can restore the parent's
   paint order on close (begin/end_window drive cur_z, which is a single global). */
u32
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

void
draw_set_viewport( u32 vp )
{
    s_draw.cur_vp = vp;
}

/* Current viewport -- saved/restored by the popup layer alongside the sort key, so an overlay
   begun mid-window leaves the parent's routing intact (begin/end_window drive cur_vp globally). */
u32
draw_viewport( void )
{
    return s_draw.cur_vp;
}

/* draw_push_clip_root -- push the full-display clip (clip_stack[0]) as a fresh top, WITHOUT
   intersecting the current clip.  A popup is a top-level overlay: it must escape the enclosing
   window's clip, so the popup layer pushes this before opening the popup window (whose own clip
   then intersects the display, not the parent) and pops it after.  Balanced with draw_pop_clip_rect. */
void
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

void
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
    Corner rounding -- the ambient radius folded into filled / outlined rects.

    draw_set_rounding installs the radius (clamped non-negative); draw_clamp_rounding fits it to a
    given rect so a corner arc never exceeds half a side, and treats a sub-pixel result as 0 so thin
    bars (separators, 1px frames) stay crisply square.  The item / chrome seams drive the radius from
    the resolved rounding category, exactly as draw_set_alpha is driven; grabs and squared marks set
    it locally for one sub-element via draw_set_rounding / draw_rounding (save + restore).
----------------------------------------------------------------------------------------------*/

void
draw_set_rounding( f32 r )
{
    s_draw.rounding = r < 0.0f ? 0.0f : r;
}

/* Current ambient radius -- so a site can save it, draw a sub-element with a different radius (a
   squared-off mark, a grab), and restore, without re-deriving the category. */
f32
draw_rounding( void )
{
    return s_draw.rounding;
}

static f32
draw_clamp_rounding( f32 w, f32 h )
{
    f32 r  = s_draw.rounding;
    f32 hw = ( w < 0.0f ? -w : w ) * 0.5f;
    f32 hh = ( h < 0.0f ? -h : h ) * 0.5f;
    if ( r > hw ) r = hw;
    if ( r > hh ) r = hh;
    return r < 0.5f ? 0.0f : r;   /* sub-pixel radius -> square fast path */
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_filled -- emit a filled / textured quad semantic command.

    tex_idx == 0 is the solid-color convention (resolved to the atlas white texel at tessellation
    time).  Pixel-grid snapping and GPU batching happen at flush time in the tessellation pass.
----------------------------------------------------------------------------------------------*/

void
draw_push_rect_filled( f32 x, f32 y, f32 w, f32 h,
                       f32 u0, f32 v0, f32 u1, f32 v1,
                       u32 tex_idx, u32 abgr )
{
    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    imgui_cmd_t* c  = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type         = IMGUI_CMD_RECT_FILLED;
    c->clip         = draw_current_clip();
    c->z            = s_draw.cur_z;
    c->vp           = s_draw.cur_vp;
    c->rect.x       = x;
    c->rect.y       = y;
    c->rect.w       = w;
    c->rect.h       = h;
    c->rect.u0      = u0;
    c->rect.v0      = v0;
    c->rect.u1      = u1;
    c->rect.v1      = v1;
    c->rect.tex_idx = tex_idx;
    c->rect.abgr    = draw_apply_alpha( abgr );
    /* Round solid-color fills only; a textured quad (glyph / image) keeps square UVs. */
    c->rect.rounding = ( tex_idx == 0 ) ? draw_clamp_rounding( w, h ) : 0.0f;
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_gradient -- emit a two-color gradient rectangle as one semantic command.

    col_a / col_b sit on opposite edges (horizontal = left->right, else top->bottom); the GPU
    interpolates the per-vertex color between them, so one quad replaces the old banded fill.
    Always square (no rounding) -- the per-vertex blend has no rounded-fan variant.
----------------------------------------------------------------------------------------------*/

void
draw_push_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    imgui_cmd_t* c          = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type                 = IMGUI_CMD_RECT_GRADIENT;
    c->clip                 = draw_current_clip();
    c->z                    = s_draw.cur_z;
    c->vp                   = s_draw.cur_vp;
    c->gradient.x           = x;
    c->gradient.y           = y;
    c->gradient.w           = w;
    c->gradient.h           = h;
    c->gradient.col_a       = draw_apply_alpha( col_a );
    c->gradient.col_b       = draw_apply_alpha( col_b );
    c->gradient.horizontal  = horizontal;
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_outline -- emit a hollow rectangle semantic command.
----------------------------------------------------------------------------------------------*/

void
draw_push_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 tex_idx, u32 abgr )
{
    (void)tex_idx;   /* outlines are always solid-color; tessellation uses the white texel */
    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    imgui_cmd_t* c       = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type              = IMGUI_CMD_RECT_OUTLINE;
    c->clip              = draw_current_clip();
    c->z                 = s_draw.cur_z;
    c->vp                = s_draw.cur_vp;
    c->rect_outline.x    = x;
    c->rect_outline.y    = y;
    c->rect_outline.w    = w;
    c->rect_outline.h    = h;
    c->rect_outline.t    = t;
    c->rect_outline.abgr = draw_apply_alpha( abgr );
    c->rect_outline.rounding = draw_clamp_rounding( w, h );
}

/*----------------------------------------------------------------------------------------------
    draw_push_triangle -- emit a solid triangle semantic command.
----------------------------------------------------------------------------------------------*/

void
draw_push_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 tex_idx, u32 abgr )
{
    (void)tex_idx;   /* triangles are always solid-color */
    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    imgui_cmd_t* c = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type        = IMGUI_CMD_TRIANGLE;
    c->clip        = draw_current_clip();
    c->z           = s_draw.cur_z;
    c->vp          = s_draw.cur_vp;
    c->tri.ax      = ax; c->tri.ay = ay;
    c->tri.bx      = bx; c->tri.by = by;
    c->tri.cx      = cx; c->tri.cy = cy;
    c->tri.abgr    = draw_apply_alpha( abgr );
}

/*----------------------------------------------------------------------------------------------
    draw_push_circle_filled -- emit a filled disc semantic command.
----------------------------------------------------------------------------------------------*/

void
draw_push_circle_filled( f32 cx, f32 cy, f32 r, u32 segments, u32 abgr )
{
    if ( segments < 3 ) segments = 3;
    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    imgui_cmd_t* c = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type        = IMGUI_CMD_CIRCLE_FILLED;
    c->clip        = draw_current_clip();
    c->z           = s_draw.cur_z;
    c->vp          = s_draw.cur_vp;
    c->circle.cx   = cx;
    c->circle.cy   = cy;
    c->circle.r    = r;
    c->circle.segs = segments;
    c->circle.abgr = draw_apply_alpha( abgr );
}

/*----------------------------------------------------------------------------------------------
    draw_push_text -- emit a glyph-run semantic command.

    str must remain valid until imgui_render_flush (same-frame caller-owned lifetime).
    n == 0xFFFFFFFF means "entire NUL-terminated string"; a smaller n limits the glyph count
    (used to skip "##label" suffixes).
----------------------------------------------------------------------------------------------*/

void
draw_push_text_clip_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    if ( !str || s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;

    /* Resolve length at push time (sentinel means NUL-terminated). */
    u32 len = ( n == 0xFFFFFFFFu ) ? (u32)strlen( str ) : n;

    /* Copy into the text pool so callers can use stack-local buffers (textf, snprintf labels).
       The pool pointer is valid until draw_reset clears it at the top of the next new_frame. */
    if ( s_draw.text_pool_used + len + 1 > IMGUI_MAX_TEXT_POOL )
        return;   /* pool exhausted: drop the label rather than store a dangling pointer */
    char* dst = s_draw.text_pool + s_draw.text_pool_used;
    memcpy( dst, str, len );
    dst[ len ]            = '\0';
    s_draw.text_pool_used += len + 1;

    imgui_cmd_t* c  = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type         = IMGUI_CMD_TEXT;
    c->clip         = draw_current_clip();
    c->z            = s_draw.cur_z;
    c->vp           = s_draw.cur_vp;
    c->text.x       = x;
    c->text.y       = y;
    c->text.str     = dst;
    c->text.len     = len;   /* always an explicit byte count; never 0xFFFFFFFF after this point */
    c->text.clip_x0 = clip_x0;
    c->text.clip_x1 = clip_x1;
    c->text.abgr    = draw_apply_alpha( abgr );
}

/* Unclipped text: the common path.  Forwards to the clipped emitter with the no-clip sentinel so
   the tessellator skips the per-glyph clip test entirely. */
void
draw_push_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n )
{
    draw_push_text_clip_n( x, y, abgr, str, n, -IMGUI_TEXT_NO_CLIP, IMGUI_TEXT_NO_CLIP );
}

void
draw_push_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text_n( x, y, abgr, str, 0xFFFFFFFFu );
}

// clang-format on
/*============================================================================================*/
