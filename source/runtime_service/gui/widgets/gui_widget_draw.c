/*==============================================================================================

    runtime_service/gui/widgets/gui_widget_draw.c -- Custom-draw / canvas escape hatches.

    Placement primitives for a rect the caller already holds, rather than a self-laying-out
    control: canvas() reserves the rect (a cell like any widget); draw_rect / draw_text are the
    raw fill/text calls; invisible_button adds click interaction on top of an explicit rect;
    text_size / text_h / draw_text_in / draw_text_clipped measure and place text within one; the
    icon section is the thin public surface over the runtime icon atlas (gui_icon.c, backend
    unit).  None of these consume the row template beyond canvas() and image() -- they act on a
    rect, not on widget_next_rect's cursor -- so they compose with any custom layout.

    Included by gui.c right after gui_widget.c; needs nothing beyond gui_widget_core.c's
    widget_next_rect / widget_id / widget_behavior and the WIDGET_/COL_ macros already in scope.

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
    return widget_next_rect( height );
}

/*----------------------------------------------------------------------------------------------
    Low-level draw_rect / draw_text
----------------------------------------------------------------------------------------------*/

void
gui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

void
gui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text( x, y, abgr, str );
}

/*----------------------------------------------------------------------------------------------
    invisible_button -- standard button interaction (hover, press-capture, click) on an explicit rect
    you already hold: a cell cut from a canvas(), a dummy() slot, any custom-drawn region.  Returns
    true on the click frame.  It owns no layout reservation (the rect is the caller's), so it composes
    with the rect helpers: cut/draw the region, then make it clickable.  For just a hover tint use
    is_mouse_hovering_rect; this adds the full press + release-on-target click semantics.
----------------------------------------------------------------------------------------------*/

bool
gui_invisible_button( const char* id_str, gui_rect_t r )
{
    gui_id_t     id = widget_id( id_str );
    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    Text measurement + aligned draw -- the placement primitives for custom drawing.  draw_text gives
    a top-left anchor only; these let a caller size to text and place it within a rect by intent
    (right-align a caption, center a label) instead of hand-computing the anchor -- the arithmetic
    that silently overflows when a constant is wrong.
----------------------------------------------------------------------------------------------*/

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

/* text_h -- the laid-out pixel height of s (text_size( s ).y); the height twin of text_w, for sizing
   a row to a (possibly multi-line) caption without taking the whole vec2. */
f32 gui_text_h( const char* s ) { return gui_text_size( s ).y; }

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

/*----------------------------------------------------------------------------------------------
    Icons -- thin public surface over the runtime icon atlas (gui_icon.c, backend unit).

    register_icon / find_icon / icon_size pass straight through; image is a layout widget that
    reserves a box and fills it; draw_icon_in is the custom-draw placement primitive (the icon
    analogue of draw_text_in) for a rect the caller already holds -- a table cell, a button label,
    a canvas cut.  Both draw helpers aspect-fit the icon centered in the rect so a non-square box
    never stretches the art, and default a 0 color to opaque white (icons are usually drawn plain).
----------------------------------------------------------------------------------------------*/

gui_icon_id_t
gui_register_icon( const char* name, u32 w, u32 h, const u8* coverage )
{
    return icon_register( name, w, h, coverage );
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
    gui_rect_t r = widget_next_rect_w( w, h );   /* reserve a w x h layout slot (like dummy) */
    gui_draw_icon_in( r, id, col );
}

/* The public gui_render_* symbol surface (draw_check_mark / draw_arrow / draw_frame /
   draw_round_rect / ... and the set_*_style setters) lives in gui_symbol.c, beside the
   draw_* helpers it wraps. */

// clang-format on
/*============================================================================================*/
