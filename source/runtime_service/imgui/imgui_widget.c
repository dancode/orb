/*==============================================================================================

    runtime_service/imgui/imgui_widget.c -- Immediate-mode widget library.

    All widgets are positioned by the layout cursor (s_ctx.cursor_x / cursor_y) which
    starts at the content area top-left of the active window and advances downward.
    Every widget advances cursor_y by its height + WIDGET_GAP after drawing.

    Widget interaction uses the classic hot/active/focused state machine:
        hot    : mouse is hovering over the widget
        active : primary mouse button is held with this widget as the target
        focused: this widget owns keyboard input (input_text)

    Included by imgui.c after imgui_ctx.c so s_ctx, s_io, s_draw, and draw helpers
    are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout accessors  (read from s_layout, computed by layout_compute() in imgui.c)
----------------------------------------------------------------------------------------------*/

#define WIDGET_H      ( (f32)s_layout.line_size      )
#define WIDGET_GAP    ( (f32)s_layout.widget_gap     )
#define WIDGET_PAD    ( (f32)s_layout.widget_pad     )
#define WIN_TITLE_H   ( (f32)s_layout.win_title_h    )
#define WIN_BORDER    ( (f32)s_layout.win_border     )
#define CHECKBOX_SZ   ( (f32)s_layout.checkbox_sz    )
#define SLIDER_KNOB_W ( (f32)s_layout.slider_knob_w  )

/*----------------------------------------------------------------------------------------------
    Color palette (IMGUI_COLOR: byte order R,G,B,A in memory = ABGR u32)
----------------------------------------------------------------------------------------------*/

#define COL_WIN_BG       IMGUI_COLOR( 0x20, 0x20, 0x20, 0xE8 )
#define COL_TITLE_BG     IMGUI_COLOR( 0x10, 0x60, 0xA0, 0xFF )
#define COL_BORDER       IMGUI_COLOR( 0x80, 0x80, 0x80, 0xFF )
#define COL_TEXT         IMGUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF )
#define COL_TEXT_DIM     IMGUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF )
#define COL_WIDGET_BG    IMGUI_COLOR( 0x40, 0x40, 0x40, 0xFF )
#define COL_WIDGET_HOT   IMGUI_COLOR( 0x50, 0x80, 0xB0, 0xFF )
#define COL_WIDGET_ACT   IMGUI_COLOR( 0x30, 0x60, 0x90, 0xFF )
#define COL_WIDGET_FG    IMGUI_COLOR( 0x20, 0x90, 0xD0, 0xFF )
#define COL_CHECK_MARK   IMGUI_COLOR( 0x20, 0xC0, 0x60, 0xFF )
#define COL_SLIDER_TRACK IMGUI_COLOR( 0x30, 0x30, 0x30, 0xFF )
#define COL_INPUT_BG     IMGUI_COLOR( 0x38, 0x38, 0x38, 0xFF )
#define COL_INPUT_FOCUS  IMGUI_COLOR( 0x20, 0x50, 0x70, 0xFF )
#define COL_CURSOR       IMGUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF )

/*----------------------------------------------------------------------------------------------
    Helpers
----------------------------------------------------------------------------------------------*/

static f32 widget_right( void ) { return s_ctx.content_x + s_ctx.content_w; }

/* Begin a new widget row; returns the rect for the full widget. */
static imgui_rect_t
widget_next_rect( f32 h )
{
    imgui_rect_t r = {
        .x = s_ctx.content_x,
        .y = s_ctx.cursor_y,
        .w = s_ctx.content_w,
        .h = h,
    };
    s_ctx.cursor_y += h + WIDGET_GAP;
    return r;
}

/* Update hot/active for a simple non-keyboard widget. */
static void
widget_interact( imgui_id_t id, imgui_rect_t r )
{
    if ( rect_hit( r ) )
        s_ctx.hot_id = id;

    /* Capture on press if hot. */
    if ( s_ctx.hot_id == id && s_io.mouse_pressed[ 0 ] )
         s_ctx.active_id = id;
}

/* Returns true when the widget was clicked (pressed + released while hot). */
static bool
widget_clicked( imgui_id_t id )
{
    return s_io.mouse_released[ 0 ]
        && s_ctx.hot_id    == id
        && s_ctx.active_id == id;
}

/* Color for a pushbutton-style widget. */
static u32
button_color( imgui_id_t id )
{
    if ( s_ctx.active_id == id ) return COL_WIDGET_ACT;
    if ( s_ctx.hot_id    == id ) return COL_WIDGET_HOT;
    return COL_WIDGET_BG;
}

/*----------------------------------------------------------------------------------------------
    begin_window / end_window
----------------------------------------------------------------------------------------------*/

void
imgui_begin_window( const char* title, f32 x, f32 y, f32 w, f32 h )
{
    s_ctx.win_x      = x;
    s_ctx.win_y      = y;
    s_ctx.win_w      = w;
    s_ctx.win_h      = h;
    s_ctx.content_x  = x + WIDGET_PAD;
    s_ctx.content_w  = w - 2.0f * WIDGET_PAD;

    /* Window background. */
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, COL_WIN_BG );

    /* Title bar. */
    draw_push_rect_filled( x, y, w, WIN_TITLE_H, 0,0,1,1, 0, COL_TITLE_BG );
    draw_push_text( x + WIDGET_PAD, y + ( WIN_TITLE_H - font_char_h() ) * 0.5f,
                    COL_TEXT, title );

    /* Border. */
    draw_push_rect_outline( x, y, w, h, WIN_BORDER, 0, COL_BORDER );

    /* Clip content area. */
    draw_push_clip_rect( x + WIN_BORDER, y + WIN_TITLE_H,
                         w - 2.0f * WIN_BORDER, h - WIN_TITLE_H - WIN_BORDER );

    /* Start the layout cursor at the content origin. */
    s_ctx.cursor_x = x + WIDGET_PAD;
    s_ctx.cursor_y = y + WIN_TITLE_H + WIDGET_GAP;
}

void
imgui_end_window( void )
{
    draw_pop_clip_rect();
}

/*----------------------------------------------------------------------------------------------
    text
----------------------------------------------------------------------------------------------*/

void
imgui_text( const char* str )
{
    imgui_rect_t r = widget_next_rect( font_char_h() );
    draw_push_text( r.x, r.y, COL_TEXT, str );
}

/*----------------------------------------------------------------------------------------------
    button -- returns true on the frame the button is released while hovered
----------------------------------------------------------------------------------------------*/

bool
imgui_button( const char* label )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_interact( id, r );

    /* Background. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, button_color( id ) );

    /* Centered label. */
    f32 lw = font_text_w( label );
    f32 lx = r.x + ( r.w - lw  ) * 0.5f;
    f32 y_shift = ( r.h - font_char_h() ) * 0.5f;
    f32 ly = r.y + y_shift;
    draw_push_text( lx, ly, COL_TEXT, label );

    return widget_clicked( id );
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
imgui_checkbox( const char* label, bool* v )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_interact( id, r );

    /* Box on the left. */
    f32 bx = r.x;
    f32 by = r.y + ( r.h - CHECKBOX_SZ ) * 0.5f;
    draw_push_rect_filled( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, 0,0,1,1, 0, COL_WIDGET_BG );
    draw_push_rect_outline( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, WIN_BORDER, 0, COL_BORDER );

    if ( *v )
    {
        /* Check mark: simple smaller filled square. */
        f32 pad = (f32)s_layout.checkmark_pad;
        draw_push_rect_filled( bx + pad, by + pad,
                               CHECKBOX_SZ - 2.0f * pad, CHECKBOX_SZ - 2.0f * pad,
                               0,0,1,1, 0, COL_CHECK_MARK );
    }

    /* Label to the right of the box. */
    draw_push_text( bx + CHECKBOX_SZ + WIDGET_PAD, r.y + ( r.h - font_char_h() ) * 0.5f,
                    COL_TEXT, label );

    bool changed = false;
    if ( widget_clicked( id ) )
    {
        *v    = !( *v );
        changed = true;
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
    slider_float -- draggable horizontal slider; returns true while dragging
----------------------------------------------------------------------------------------------*/

bool
imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Label to the right of the track; track takes the left portion. */
    f32 label_w = font_text_w( label );
    f32 track_w = r.w - label_w - WIDGET_PAD;
    f32 min_w   = (f32)( s_layout.slider_knob_w * 3u );
    if ( track_w < min_w ) track_w = min_w;

    imgui_rect_t track_r = { r.x, r.y, track_w, r.h };
    widget_interact( id, track_r );

    /* Drag: update value when active. */
    bool changed = false;
    if ( s_ctx.active_id == id )
    {
        f32 t = ( s_io.mouse_x - track_r.x ) / track_r.w;
        t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
        f32 nv = lo + t * ( hi - lo );
        if ( nv != *v )
        {
            *v     = nv;
            changed = true;
        }
    }

    /* Draw track. */
    draw_push_rect_filled( track_r.x, track_r.y, track_r.w, track_r.h,
                           0,0,1,1, 0, COL_SLIDER_TRACK );
    draw_push_rect_outline( track_r.x, track_r.y, track_r.w, track_r.h,
                            WIN_BORDER, 0, COL_BORDER );

    /* Draw fill bar up to the current value. */
    f32 t_cur    = ( hi > lo ) ? ( ( *v - lo ) / ( hi - lo ) ) : 0.0f;
    f32 fill_w   = t_cur * ( track_r.w - SLIDER_KNOB_W );
    if ( fill_w > 0.0f )
        draw_push_rect_filled( track_r.x, track_r.y + 1.0f, fill_w, track_r.h - 2.0f,
                               0,0,1,1, 0, COL_WIDGET_FG );

    /* Draw knob. */
    f32 knob_x = track_r.x + t_cur * ( track_r.w - SLIDER_KNOB_W );
    draw_push_rect_filled( knob_x, track_r.y, SLIDER_KNOB_W, track_r.h,
                           0,0,1,1, 0, button_color( id ) );

    /* Label. */
    draw_push_text( track_r.x + track_r.w + WIDGET_PAD,
                    r.y + ( r.h - font_char_h() ) * 0.5f,
                    COL_TEXT, label );

    return changed;
}

/*----------------------------------------------------------------------------------------------
    input_text -- single-line text field; returns true when Enter is pressed
----------------------------------------------------------------------------------------------*/

bool
imgui_input_text( const char* label, char* buf, u32 bufsz )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Box takes the left portion; label on the right. */
    f32 label_w = font_text_w( label );
    f32 box_w    = r.w - label_w - WIDGET_PAD;
    f32 min_box  = s_font->char_h * 3.0f;
    if ( box_w < min_box ) box_w = min_box;
    imgui_rect_t box_r = { r.x, r.y, box_w, r.h };

    /* Click focuses this widget. */
    if ( rect_hit( box_r ) )
        s_ctx.hot_id = id;
    if ( s_ctx.hot_id == id && s_io.mouse_pressed[ 0 ] )
        s_ctx.focused_id = id;

    bool focused = ( s_ctx.focused_id == id );
    bool enter   = false;

    /* Text input when focused. */
    if ( focused )
    {
        /* Printable chars from s_io.text. */
        for ( const char* ch = s_io.text; *ch; ++ch )
        {
            u32 len = 0;
            while ( len < bufsz - 1 && buf[ len ] ) ++len;
            if ( len + 1 < bufsz )
            {
                buf[ len     ] = *ch;
                buf[ len + 1 ] = '\0';
            }
        }

        /* Backspace. */
        if ( s_io.keys_pressed[ APP_KEY_BACKSPACE ] )
        {
            u32 len = 0;
            while ( len < bufsz - 1 && buf[ len ] ) ++len;
            if ( len > 0 )
                buf[ len - 1 ] = '\0';
        }

        /* Enter submits. */
        if ( s_io.keys_pressed[ APP_KEY_ENTER ] )
        {
            s_ctx.focused_id = IMGUI_ID_NONE;
            enter = true;
        }

        /* Escape cancels focus. */
        if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
            s_ctx.focused_id = IMGUI_ID_NONE;
    }

    /* Background. */
    draw_push_rect_filled( box_r.x, box_r.y, box_r.w, box_r.h,
                           0,0,1,1, 0,
                           focused ? COL_INPUT_FOCUS : COL_INPUT_BG );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h,
                            WIN_BORDER, 0,
                            focused ? COL_WIDGET_HOT : COL_BORDER );

    /* Buffer contents. */
    draw_push_text( box_r.x + WIDGET_PAD,
                    box_r.y + ( box_r.h - font_char_h() ) * 0.5f,
                    COL_TEXT, buf );

    /* Blinking cursor (always visible when focused for simplicity). */
    if ( focused )
    {
        f32 tw      = font_text_w( buf );
        f32 cx      = box_r.x + WIDGET_PAD + tw;
        f32 inset   = (f32)s_layout.cursor_inset;
        f32 cur_w   = (f32)s_layout.cursor_w;
        if ( cx + cur_w < box_r.x + box_r.w )
            draw_push_rect_filled( cx, box_r.y + inset,
                                   cur_w, box_r.h - inset * 2.0f,
                                   0,0,1,1, 0, COL_CURSOR );
    }

    /* Label. */
    draw_push_text( box_r.x + box_r.w + WIDGET_PAD,
                    r.y + ( r.h - font_char_h() ) * 0.5f,
                    COL_TEXT_DIM, label );

    return enter;
}

/*----------------------------------------------------------------------------------------------
    Low-level draw_rect / draw_text
----------------------------------------------------------------------------------------------*/

void
imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

void
imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text( x, y, abgr, str );
}

// clang-format on
/*============================================================================================*/
