/*==============================================================================================

    runtime_service/imgui/imgui_widget.c -- Leaf widgets.

    The everyday controls a caller emits between begin_window / end_window: text, button,
    checkbox, slider, text input, plus the low-level draw_rect / draw_text escape hatches.
    Each takes its rect from widget_next_rect, which carves the next cell out of the active
    region's row template (imgui_widget_core.c) -- a plain vertical stack by default, or any
    multi-column shape set via imgui_layout / the row sugar.  The widget just fills the rect.

    The window itself is a compound widget and lives in imgui_widget_window.c; the shared
    interaction state machine, theme, and layout macros these widgets build on live in
    imgui_widget_core.c.

    Included by imgui.c after imgui_widget_core.c so widget_behavior, widget_next_rect, the
    COL_* palette, and the WIDGET_/WIN_ layout macros are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    text
----------------------------------------------------------------------------------------------*/

void
imgui_text( const char* str )
{
    imgui_rect_t r = widget_next_rect( font_char_h() );
    draw_push_text( r.x, r.y, COL_TEXT, str );
    widget_track_width( r.x + font_text_w( str ) );   /* natural width may exceed the row */
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
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Background. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    /* Centered label. */
    f32 lw = label_width( label );
    f32 lx = r.x + ( r.w - lw  ) * 0.5f;
    draw_label( lx, text_center_y( r.y, r.h ), COL_TEXT, label );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
imgui_checkbox( const char* label, bool* v )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Field split mode aligns with the other labeled widgets: the label takes its track and the
       box sits at the start of the control track.  Default mode keeps the box on the left with the
       label trailing it.  The box only needs CHECKBOX_SZ of the control track. */
    f32          label_x;
    imgui_rect_t control;
    f32          bx;
    if ( field_split_resolve( r, CHECKBOX_SZ, &label_x, &control ) )
    {
        bx = control.x;
    }
    else
    {
        bx      = r.x;
        label_x = bx + CHECKBOX_SZ + WIDGET_PAD;   /* default: label just right of the box */
    }

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

    draw_label( label_x, text_center_y( r.y, r.h ), COL_TEXT, label );

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
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Track takes the left portion; the label sits at the right.  The min track width keeps the
       knob travel usable when the label is long. */
    imgui_rect_t track_r = widget_split_label( r, label, (f32)( s_layout.slider_knob_w * 3u ), COL_TEXT );
    widget_state_t st = widget_behavior( id, track_r, WIDGET_KIND_DRAG );

    /* Drag: update value when active. */
    bool changed = false;
    if ( st.active )
    {
        f32 t = saturate( ( s_io.mouse_x - track_r.x ) / track_r.w );
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

    return changed;
}

/*----------------------------------------------------------------------------------------------
    input_text -- single-line text field; returns true when Enter is pressed
----------------------------------------------------------------------------------------------*/

bool
imgui_input_text( const char* label, char* buf, u32 bufsz )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Box takes the left portion; the (dim) label sits at the right.  Min box width keeps a
       few glyphs of edit space visible when the label is long. */
    imgui_rect_t box_r = widget_split_label( r, label, s_font->char_h * 3.0f, COL_TEXT_DIM );

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
    draw_push_text( box_r.x + WIDGET_PAD, text_center_y( box_r.y, box_r.h ), COL_TEXT, buf );

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

    return enter;
}

/*----------------------------------------------------------------------------------------------
    selectable -- a full-width row that highlights on hover and fills when selected.

    The building block for list boxes: emit one per item (typically inside a begin_child
    region so they scroll and clip independently).  When `selected` is non-NULL a click
    toggles it; pass NULL for a click-only row.  Returns true on the frame it is clicked, so
    a caller managing single-selection can set its own index from the return without relying
    on the toggle.
----------------------------------------------------------------------------------------------*/

bool
imgui_selectable( const char* label, bool* selected )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Fill: selected rows use the active tint, a hovered row the hot tint; otherwise the row
       is transparent so the region background shows through. */
    bool on = ( selected && *selected );
    if ( on || st.hover )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0,
                               on ? COL_WIDGET_ACT : COL_WIDGET_HOT );

    /* Label, left-aligned with the standard padding. */
    draw_label( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label );
    widget_track_width( r.x + WIDGET_PAD + label_width( label ) );   /* natural width may exceed the row */

    if ( st.clicked && selected )
        *selected = !( *selected );
    return st.clicked;
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
