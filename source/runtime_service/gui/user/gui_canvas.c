/*==============================================================================================

    runtime_service/gui/user/gui_canvas.c -- Custom-draw / canvas surface of the user tier.

    Placement primitives for a rect the caller already holds, rather than a self-laying-out
    control: canvas() reserves the rect (a cell like any widget); draw_rect / draw_text are the
    raw fill/text calls; text_size / draw_text_in / draw_text_clipped measure and place text
    within one; the icon section is the thin public surface over the runtime icon atlas
    (gui_icon.c, backend unit).  None of these consume the row template beyond canvas() and
    image() -- they act on a rect, not on cell_next's cursor -- so they compose with any
    custom layout.  The interaction half of the tier (gui_item / invisible_button) is
    gui_behavior.c, included beside it; together they are the substrate a user widget is written on.

    Included by gui.c in the user/ tier (last of the tiers -- pure vocabulary, no state); needs
    only cell_next / rect_align (core+compose) and the draw_push_* backend calls, all in
    scope far above.

==============================================================================================*/
// clang-format off

/* Reserve a rectangular drawing area in the layout and hand back its screen rect, for custom
   geometry (draw_line / draw_polyline / draw_rect / draw_text).  Consumes one cell like any
   widget -- full content width, `height` pixels tall (height <= 0 fills the remaining region
   height) -- so it flows in the vertical list and the pen resumes below it.  The returned rect is
   in the same screen space the draw_* calls take, and the enclosing window clips it. */
gui_rect_t
gui_canvas( f32 height )
{
    if ( height <= 0.0f )
        height = gui_content_avail().y;
    return cell_next( height );
}

/*==============================================================================================
    Low-level draw_rect / draw_text
==============================================================================================*/

void
gui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

/* Batched form: N solid rects as ONE semantic command (one quad each at flush).  For dense
   custom drawing -- timeline bars, graph columns, heatmap cells -- where per-rect draw_rect
   calls would exhaust the frame's command budget (GUI_MAX_CMDS).  Entries share the current
   clip; square corners; per-entry color. */
void
gui_draw_rects( const gui_rect_col_t* rects, u32 count )
{
    draw_push_rect_list( rects, count );
}

void
gui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text( x, y, abgr, str );
}

/* Volatile widgets (gui()->volatile_cb / volatile_begin / volatile_end) live in their own file --
   widgets/gui_volatile.c -- rather than here, since they're a distinct cross-cutting feature
   spanning both units, not a custom-draw escape hatch. */

/* invisible_button / gui_item -- interaction on an explicit rect -- moved to gui_behavior.c,
   the behavior half of this tier. */

/*==============================================================================================
    Text measurement + aligned draw -- the placement primitives for custom drawing.  draw_text gives
    a top-left anchor only; these let a caller size to text and place it within a rect by intent
    (right-align a caption, center a label) instead of hand-computing the anchor -- the arithmetic
    that silently overflows when a constant is wrong.
==============================================================================================*/

/* text_size -- the laid-out pixel size of s as draw_text_in / draw_text render it: width is the
   widest line, height spans the lines ('\n' breaks; one line is char_h, each extra adds a full line
   advance).  The CalcTextSize analogue, for sizing a rect or centering by hand. */
gui_vec2_t
gui_text_size( const char* s )
{
    if ( !s ) return ( gui_vec2_t ){ 0.0f, 0.0f };

    f32         max_w = 0.0f;
    u32         lines = 1;
    const char* line  = s;
    for ( const char* p = s; ; ++p )
    {
        if ( *p == '\n' || *p == '\0' )
        {
            f32 w = font_text_w_n( line, (u32)( p - line ) );
            if ( w > max_w ) max_w = w;
            if ( *p == '\0' ) break;
            ++lines;
            line = p + 1;
        }
    }
    return ( gui_vec2_t ){ max_w, font_char_h() + (f32)( lines - 1 ) * font_line_h() };
}

/* draw_text_in -- draw s aligned within rect r (gui_align_t).  Multi-line: the block is placed by
   the vertical flag, each line by the horizontal flag, so RIGHT flushes every line to r's right edge. */
void
gui_draw_text_in( gui_rect_t r, gui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32         y    = align_y( r.y, r.h, gui_text_size( s ).y, align );
    const char* line = s;
    for ( const char* p = s; ; ++p )
    {
        if ( *p == '\n' || *p == '\0' )
        {
            u32 n = (u32)( p - line );
            draw_push_text_n( align_x( r.x, r.w, font_text_w_n( line, n ), align ), y, col, line, n );
            if ( *p == '\0' ) break;
            y   += font_line_h();
            line = p + 1;
        }
    }
}

/* draw_text_clipped -- single-line draw_text_in that ellipsizes to r's width when s does not fit
   (the fitted run is left-anchored; alignment applies only while it fits). */
void
gui_draw_text_clipped( gui_rect_t r, gui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32 w = font_text_w( s );
    f32 y = align_y( r.y, r.h, font_char_h(), align );
    if ( w <= r.w )
        draw_push_text( align_x( r.x, r.w, w, align ), y, col, s );
    else
        draw_text_fit_n( r.x, y, col, s, 0xFFFFFFFFu, r.w );
}

/*==============================================================================================
    Icons -- thin public surface over the runtime icon atlas (gui_icon.c, backend unit).

    register_icon / load_icon / find_icon / icon_size pass straight through; image is a layout widget that
    reserves a box and fills it; draw_icon_in is the custom-draw placement primitive (the icon
    analogue of draw_text_in) for a rect the caller already holds -- a table cell, a button label,
    a canvas cut.  Both draw helpers aspect-fit the icon centered in the rect so a non-square box
    never stretches the art, and default a 0 color to opaque white (icons are usually drawn plain).
==============================================================================================*/

gui_icon_id_t
gui_register_icon( const char* name, u32 w, u32 h, const u8* coverage )
{
    return icon_register( name, w, h, coverage );
}

gui_icon_id_t
gui_load_icon( const char* name, const char* path )
{
    char resolved[ 576 ];
    fmt_snprintf( resolved, sizeof( resolved ), "%s/%s", sys_root_dir(), path );
    return icon_load_file( name, resolved );
}

gui_icon_id_t
gui_find_icon( const char* name )
{
    return icon_find( name );
}

gui_vec2_t
gui_icon_size( gui_icon_id_t id )
{
    u32 w = 0, h = 0;
    icon_get( id, NULL, NULL, NULL, NULL, &w, &h );
    return ( gui_vec2_t ){ (f32)w, (f32)h };
}

void
gui_draw_icon_in( gui_rect_t r, gui_icon_id_t id, u32 col )
{
    u32 iw = 0, ih = 0;
    if ( !icon_get( id, NULL, NULL, NULL, NULL, &iw, &ih ) || iw == 0 || ih == 0 )
        return;

    /* Aspect-fit: scale to the tighter of the two axes, then center the fitted box in r. */
    f32 sx  = r.w / (f32)iw;
    f32 sy  = r.h / (f32)ih;
    f32 s   = sx < sy ? sx : sy;
    f32 w   = (f32)iw * s;
    f32 h   = (f32)ih * s;
    gui_rect_t box = rect_align( r, w, h, GUI_ALIGN_HCENTER | GUI_ALIGN_VCENTER );

    draw_push_icon( box.x, box.y, box.w, box.h, id, col ? col : 0xFFFFFFFFu );
}

void
gui_image( gui_icon_id_t id, f32 w, f32 h, u32 col )
{
    gui_rect_t r = cell_next_w( w, h );   /* reserve a w x h layout slot (like empty) */
    gui_draw_icon_in( r, id, col );
}

/*==============================================================================================
    RGBA textures -- display an arbitrary bindless texture (a scene render target, a loaded
    image) as a full-color quad.  Unlike image() (R8 icon atlas, texel = coverage), the texel IS
    the color: GUI_TEX_RGBA_BIT on the command's tex_idx flips the fragment shader to RGBA
    sampling, with the tint color multiplied in (0 defaults to opaque white = untinted).  The
    caller owns the texture and its bindless registration (rhi register_texture) and must keep
    the slot alive until the frame that last referenced it has retired.
==============================================================================================*/

void
gui_draw_texture_in( gui_rect_t r, u32 bindless_idx, u32 tint_abgr )
{
    if ( bindless_idx == 0 )
        return;   /* 0 is the RHI empty slot -- nothing to sample */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0, 0, 1, 1,
                           bindless_idx | GUI_TEX_RGBA_BIT,
                           tint_abgr ? tint_abgr : 0xFFFFFFFFu );
}

void
gui_image_texture( u32 bindless_idx, f32 w, f32 h, u32 tint_abgr )
{
    gui_rect_t r = cell_next_w( w, h );   /* reserve a w x h layout slot (like image) */
    gui_draw_texture_in( r, bindless_idx, tint_abgr );
}

/* Font atlas access -- bridges the font registry (gui_font.h / gui_backend.h) to the RGBA texture
   primitives above, so a caller can preview a font's live GPU atlas (a texture like any other) via
   image_texture / draw_texture_in without reaching into the backend's internal font_slot_t. */

u32
gui_font_atlas_idx( u32 font_id )
{
    return font_slot_atlas_idx( font_id );
}

gui_vec2_t
gui_font_atlas_size( u32 font_id )
{
    return font_slot_atlas_size( font_id );
}

/* The public gui_render_* symbol surface (draw_check_mark / draw_arrow / draw_frame /
   draw_round_rect / ... and the set_*_style setters) lives in gui_symbol.c, beside the
   draw_* helpers it wraps. */

// clang-format on
/*============================================================================================*/
