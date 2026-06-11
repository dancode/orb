/*==============================================================================================

    runtime_service/imgui/imgui_widget.c -- Immediate-mode widget library.

    All widgets are positioned by the layout cursor (s_ctx.cursor_x / cursor_y) which
    starts at the content area top-left of the active window and advances downward.
    Every widget advances cursor_y by its height + WIDGET_GAP after drawing.

    Widget interaction uses the classic hover/active/focused state machine:
        hover    : mouse is hovering over the widget
        active : primary mouse button is held with this widget as the target
        focused: this widget owns keyboard input (input_text)

    Included by imgui.c after imgui_ctx.c so s_ctx, s_io, s_draw, and draw helpers
    are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout accessors  (read from s_layout, computed by layout_compute() in imgui.c)
----------------------------------------------------------------------------------------------*/

#define WIDGET_H      ( (f32)s_layout.line_size     )
#define WIDGET_GAP    ( (f32)s_layout.widget_gap    )
#define WIDGET_PAD    ( (f32)s_layout.widget_pad    )
#define WIN_TITLE_H   ( (f32)s_layout.win_title_h   )
#define WIN_BORDER    ( (f32)s_layout.win_border    )
#define CHECKBOX_SZ   ( (f32)s_layout.checkbox_sz   )
#define SLIDER_KNOB_W ( (f32)s_layout.slider_knob_w )

/*----------------------------------------------------------------------------------------------
    Color palette (IMGUI_COLOR: byte order R,G,B,A in memory = ABGR u32)
----------------------------------------------------------------------------------------------*/

#define COL_WIN_BG       IMGUI_COLOR( 0x24, 0x24, 0x24, 0xE8 )
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

/* Interaction class for a widget, selected at the call site.  Only the press-time
   behavior differs between widgets; everything else (hover/active/click) is uniform. */
typedef enum
{
    WIDGET_KIND_BUTTON    = 0,   /* press captures active; reports clicked   */
    WIDGET_KIND_DRAG      = 1,   /* press captures active; held for dragging */
    WIDGET_KIND_FOCUSABLE = 2,   /* press also claims keyboard focus         */

} widget_kind_t;

/* Result of one frame of interaction for a widget.  Every widget drives its
   visuals and value changes from these flags instead of touching s_ctx directly. */
typedef struct
{
    bool hover;       /* cursor is over the widget this frame                  */
    bool active;    /* primary button held with this widget as the target    */
    bool pressed;   /* primary button went down on the widget this frame     */
    bool clicked;   /* press + release completed with the cursor still over  */
    bool focused;   /* widget owns keyboard input (focusable widgets)        */

} widget_state_t;

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

static widget_state_t
widget_behavior( imgui_id_t id, imgui_rect_t r, widget_kind_t kind )
{
    widget_state_t st = { 0 };

    /* Hot only when this widget belongs to the window the cursor is over (hover_win,
       resolved last frame).  Widgets in any other window short-circuit before rect_hit,
       so occluded windows do no hit-testing at all -- occlusion is decided once, at the
       window level, not per widget. */
    if ( s_ctx.win_id == s_ctx.hover_win && rect_hit( r ) )
        s_ctx.hover_id = id;

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_ctx.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_ctx.active_id = id;
        st.pressed      = true;
        if ( kind == WIDGET_KIND_FOCUSABLE )
            s_ctx.focused_id = id;
    }

    st.hover     = ( s_ctx.hover_id == id );
    st.active  = ( s_ctx.active_id == id );
    st.focused = ( s_ctx.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_ctx.hover_id == id && s_ctx.active_id == id;

    return st;
}

/* Background color for a pushbutton / knob style widget, from its interaction state. */
static u32
widget_bg_color( widget_state_t st )
{
    if ( st.active ) return COL_WIDGET_ACT;
    if ( st.hover    ) return COL_WIDGET_HOT;
    return COL_WIDGET_BG;
}

/*----------------------------------------------------------------------------------------------
    begin_window / end_window     
----------------------------------------------------------------------------------------------*/

/* Keep a dragged window reachable: clamp so its top edge stays on-screen and at
   least one title-bar's worth of the window remains within the display bounds. */
static void
window_clamp( imgui_window_t* win )
{
    const f32 margin = WIN_TITLE_H;
    const f32 max_x  = (f32)s_io.display_w - margin;
    const f32 max_y  = (f32)s_io.display_h - margin;

    if ( win->x > max_x )          win->x = max_x;
    if ( win->y > max_y )          win->y = max_y;
    if ( win->x < margin - win->w ) win->x = margin - win->w;
    if ( win->y < 0.0f )            win->y = 0.0f;
}

void
imgui_begin_window( const char* title, f32 x, f32 y, f32 w, f32 h )
{
    /* x/y/w/h are the initial geometry; the registry owns position after that. */
    imgui_id_t      id  = id_hash( title );
    imgui_window_t* win = window_get( id, x, y, w, h );

    /* Apply an in-progress drag: this window holds active_id while the button is down. */
    if ( s_ctx.active_id == id )
    {
        win->x = s_io.mouse_x - s_drag_off_x;
        win->y = s_io.mouse_y - s_drag_off_y;
        window_clamp( win );
    }

    /* Nominate this window as the one under the cursor (front-most by z wins).  The
       winner becomes hover_win next frame; that single fact gates all widget hit-testing
       and the drag grab below, so occlusion is resolved once per frame, not per widget. */
    window_nominate_hover( id, ( imgui_rect_t ){ win->x, win->y, win->w, win->h }, win->z );

    /* All of this window's geometry is stamped with its z so flush can paint
       windows back-to-front regardless of begin_window call order. */
    draw_set_sort_key( win->z );

    /* Commit resolved geometry for the widgets and end_window. */
    s_ctx.win_id     = id;
    s_ctx.win_title  = title;   /* cached for end_window's deferred chrome */
    s_ctx.win_x      = win->x;
    s_ctx.win_y      = win->y;
    s_ctx.win_w      = win->w;
    s_ctx.win_h      = win->h;
    s_ctx.content_x  = win->x + WIDGET_PAD;
    s_ctx.content_w  = win->w - 2.0f * WIDGET_PAD;

    /* One clip rect for the whole window: background, content, and the titlebar/border
       chrome deferred to end_window all share it, so the window flushes as a single draw
       command.  Content scrolled into the titlebar or border region is overpainted by the
       chrome end_window draws last; anything past the outer edge is clipped here. */

    draw_push_clip_rect( win->x, win->y, win->w, win->h );

    /* Window background. */
    draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );

    /* Start the layout cursor at the content origin. */
    s_ctx.cursor_x = win->x + WIDGET_PAD;
    s_ctx.cursor_y = win->y + WIN_TITLE_H + WIDGET_GAP;
}

void
imgui_end_window( void )
{
    /* Deferred chrome: titlebar, title text, and border paint last under the window's
       single clip rect, so they overdraw any content that scrolled beneath them while
       still merging into the one window draw command. */
    draw_push_rect_filled( s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, WIN_TITLE_H, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_TITLE_BG );
    draw_push_text( s_ctx.win_x + WIDGET_PAD, s_ctx.win_y + ( WIN_TITLE_H - font_char_h() ) * 0.5f, COL_TEXT, s_ctx.win_title );
    draw_push_rect_outline( s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, s_ctx.win_h, WIN_BORDER, 0, COL_BORDER );

    draw_pop_clip_rect();

    /* Subsequent draws (low-level API, the next window) revert to the background key. */
    draw_set_sort_key( 0 );

    /* Drag grab.  Decided here, after this window's widgets have run, so hover_id tells us
       whether the press landed on a widget: this window is the one under the cursor
       (hover_win) and no widget of it took the hover (hover_id == NONE) means the press is on
       empty window space.  BODY drags from anywhere empty; TITLEBAR only from the bar.
       The move itself starts next frame in begin_window (one-frame grab latency). */
    if ( s_ctx.win_id == s_ctx.hover_win && s_ctx.hover_id == IMGUI_ID_NONE
         && s_io.mouse_pressed[ 0 ] && s_ctx.active_id == IMGUI_ID_NONE
         && s_win_drag_mode != IMGUI_WIN_DRAG_NONE )
    {
        imgui_rect_t title_r = { s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, WIN_TITLE_H };
        if ( s_win_drag_mode == IMGUI_WIN_DRAG_BODY || rect_hit( title_r ) )
        {
            s_ctx.active_id = s_ctx.win_id;
            s_drag_off_x    = s_io.mouse_x - s_ctx.win_x;
            s_drag_off_y    = s_io.mouse_y - s_ctx.win_y;
        }
    }
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
    textf -- printf-style text label (no overloading, so distinct from text())
----------------------------------------------------------------------------------------------*/

void
imgui_textf( const char* fmt, ... )
{
    /* Format into a frame-local buffer; oversized output is truncated, not wrapped. */
    char buf[ 1024 ];

    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    imgui_text( buf );
}

/*----------------------------------------------------------------------------------------------
    button -- returns true on the frame the button is released while hovered
----------------------------------------------------------------------------------------------*/

bool
imgui_button( const char* label )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Background. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    /* Centered label. */
    f32 lw = font_text_w( label );
    f32 lx = r.x + ( r.w - lw  ) * 0.5f;
    f32 y_shift = ( r.h - font_char_h() ) * 0.5f;
    f32 ly = r.y + y_shift;
    draw_push_text( lx, ly, COL_TEXT, label );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
imgui_checkbox( const char* label, bool* v )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

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
    if ( st.clicked )
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
    widget_state_t st = widget_behavior( id, track_r, WIDGET_KIND_DRAG );

    /* Drag: update value when active. */
    bool changed = false;
    if ( st.active )
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
                           0,0,1,1, 0, widget_bg_color( st ) );

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

    /* Click focuses this widget (focus claim handled by the behavior helper). */
    widget_state_t st = widget_behavior( id, box_r, WIDGET_KIND_FOCUSABLE );

    bool focused = st.focused;
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
