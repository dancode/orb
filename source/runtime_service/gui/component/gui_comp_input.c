/*==============================================================================================

    runtime_service/gui/component/gui_comp_input.c -- single-line text field component.

    The richest component, and the clearest statement of what the tier is FOR.  The whole edit
    engine already lives in interact/ (gui_edit.c: keys, mouse-selection drag, clipboard, undo,
    horizontal scroll, caret blink) behind edit_field().  This component runs it and translates
    the engine's internal, measurement-laden state into PAINTABLE GEOMETRY -- a content rect to
    clip to, the run's draw origin, and the selection / caret bars already positioned -- so a
    render draws a text field with only public verbs (push_clip / draw_text / draw_rect) and never
    touches gui_edit_state_t or measures a glyph.

    It measures (text_x_at over the active font) but never paints -- measurement is logic (sizes
    and rects), the foundation layout stands on, not drawing.  It hit-tests the whole rect (a
    press anywhere focuses) and edits within the rect inset by `pad` (the render's chosen text
    margin), mirroring how the engine wants a content rect free of widget padding.

==============================================================================================*/

// clang-format off

/* One text field over a caller rect: claim focus on press, run the edit engine in the padded
   content rect, then resolve the paint geometry from the keyed edit state the engine settled.
   The engine owns every buffer mutation and its own redraw; this returns changed / enter plus the
   bars a render fills.  A hidden bar (no selection, or the caret's dark blink phase) has w == 0. */
gui_comp_input_t
gui_comp_input( const char* id_str, gui_rect_t rect, f32 pad, char* buf, u32 bufsz )
{
    gui_comp_input_t out = ( gui_comp_input_t ){ 0 };

    gui_id_t         id = item_id( id_str );
    gui_item_state_t st = item_state( id, rect, ITEM_FOCUSABLE );   /* press also claims keyboard */
    out.state = st;

    /* The engine works in a content rect inset horizontally by pad, so it never sees the widget
       margin; it leaves cursor / anchor / pan_x / blink_t on the keyed slot. */
    gui_rect_t content = { rect.x + pad, rect.y, rect.w - 2.0f * pad, rect.h };
    input_field_result_t res = edit_field( id, content, st, buf, bufsz );

    out.content = content;
    out.changed = res.changed;
    out.enter   = res.enter;

    const gui_edit_state_t* es = GUI_STATE( gui_edit_state_t, id );

    f32 text_x  = content.x - es->pan_x;
    out.text_x  = text_x;
    out.text_y  = content.y + ( content.h - font_char_h() ) * 0.5f;   /* one line, vertically centered */

    f32 clip_x0 = content.x;
    f32 clip_x1 = content.x + content.w;

    if ( st.focused )
    {
        /* Selection band, clamped to the content clip. */
        u32  sel_lo, sel_hi;
        bool has_sel;
        edit_sel( es, &sel_lo, &sel_hi, &has_sel );
        if ( has_sel )
        {
            f32 sx0 = text_x + text_x_at( buf, sel_lo );
            f32 sx1 = text_x + text_x_at( buf, sel_hi );
            if ( sx0 < clip_x0 ) sx0 = clip_x0;
            if ( sx1 > clip_x1 ) sx1 = clip_x1;
            if ( sx1 > sx0 )
                out.selection = ( gui_rect_t ){ sx0, content.y + 1.0f, sx1 - sx0, content.h - 2.0f };
        }

        /* Caret: a 1px column, visible the first half of each 1 s blink cycle. */
        if ( ( (u32)( es->blink_t * 2.0f ) & 1u ) == 0u )
        {
            f32 cx = text_x + text_x_at( buf, es->cursor );
            out.caret = ( gui_rect_t ){ cx, content.y + 2.0f, 1.0f, content.h - 4.0f };
        }
    }

    return out;
}

// clang-format on
/*============================================================================================*/
