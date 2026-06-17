/*==============================================================================================

    runtime_service/imgui/imgui_text_edit.c -- Single-line text editing engine.

    The full editing behavior behind input_text: cursor movement, selection (keyboard + mouse
    drag, double-click word select), clipboard copy / cut / paste, insertion + deletion,
    horizontal scroll to keep the caret in view, and rendering (text, selection highlight,
    blinking caret) -- all clipped to a caller-supplied box.  input_text (imgui_widget.c) is a
    thin wrapper: it owns the label split, the box background / border, and the focus claim,
    then delegates here.  Lifted out of imgui_widget_core.c so the editor is one named unit.

    The persisted per-id edit state (cursor, anchor, scroll, blink) lives in the keyed state
    pool (imgui_ctx.c); imgui_clipboard_set is in imgui_input.c.

    Included by imgui.c after imgui_widget_core.c so widget_state_t, text_center_y, the COL_*
    palette, WIDGET_PAD / WIN_BORDER, and the draw + font helpers are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Text edit state -- persisted per-id across frames by input_field_edit.

    cursor and anchor together describe the selection: the highlighted range is
    [min(cursor,anchor), max(cursor,anchor)), and cursor == anchor means no selection
    (bare insertion point).  scroll_x is the horizontal pixel bias that keeps the caret
    inside the visible portion of a narrow box.  blink_t accumulates s_io.dt and drives
    the 50/50 on-off cursor blink; it is reset to 0 on any keypress or click so the caret
    is always immediately visible after user activity.

    - Left/Right arrows with cursor-collapse on Shift-less movement over a selection
    - Home/End
    - Shift+any of the above to extend selection
    - Ctrl+A to select all
    - Ctrl+C / Ctrl+X copy / cut to the OS clipboard; paste applied from s_io.paste (the
      platform posts APP_EV_CLIPBOARD on the paste gesture; no Ctrl+V check needed here)
    - Backspace and Delete both at caret and over selection
    - Character insertion at caret, or selection replacement on first char
    - Blinking caret (reset to visible on any keypress or click)
    - Click-to-caret positioning (also fires on the focus-gaining click)
    - Click-drag to select, double-click to select the word under the cursor
    - Horizontal scroll that keeps the caret visible whenever it moves
    - Draw clip so scrolled text never bleeds past the box border

----------------------------------------------------------------------------------------------*/

typedef struct
{
    u32  cursor;    /* byte offset of the caret (insertion / deletion point) */
    u32  anchor;    /* passive end of the selection; cursor == anchor -> none */
    f32  scroll_x;  /* horizontal scroll offset in pixels                     */
    f32  blink_t;   /* seconds since last caret-visibility reset              */

} imgui_edit_state_t;

/* Both return flags from input_field_edit.  changed fires on any buffer modification;
   enter fires when the user submits the field with Enter (and focus is dropped).  They
   are independent: a paste that happens to contain a newline could set both in one frame,
   though the current implementation does not process newlines as submit signals. */
typedef struct { bool changed; bool enter; } input_field_result_t;

/* Pixel x-offset of the insertion point at byte index `off` in `buf`, measured from the
   left edge of the first glyph (scroll is not applied here; the caller adjusts).  Stops
   safely at a NUL so off > len is handled without bounds checks. */
static f32
text_x_at( const char* buf, u32 off )
{
    f32 x = 0.0f;
    for ( u32 i = 0; i < off && buf[ i ]; ++i )
        x += font_char_advance( (u8)buf[ i ] );
    return x;
}

/* Byte offset in buf[0..len) nearest to pixel position `px` measured from the text origin.
   Snaps to the midpoint of each glyph so a click in the left half of a glyph lands before
   it and in the right half lands after it, matching standard click-to-caret behaviour. */
static u32
text_offset_at( const char* buf, u32 len, f32 px )
{
    f32 x = 0.0f;
    for ( u32 i = 0; i < len; ++i )
    {
        f32 adv = font_char_advance( (u8)buf[ i ] );
        if ( px < x + adv * 0.5f ) return i;
        x += adv;
    }
    return len;
}

/* Character class for double-click word selection: a click extends over the maximal run of
   one class.  0 = whitespace, 1 = word (alphanumeric or underscore), 2 = punctuation/other --
   the classic "select word, or select a run of symbols" split. */
static int
char_class( u8 c )
{
    if ( c == ' ' || c == '\t' )                                  return 0;
    if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
         ( c >= '0' && c <= '9' ) || c == '_' )                   return 1;
    return 2;
}

/* Word bounds [*lo,*hi) around byte `off`: the run of same-class characters containing it.
   Used by double-click to snap the selection to a whole word.  A click past the end (off ==
   len) collapses to an empty range at len so a double-click in empty space selects nothing. */
static void
word_bounds( const char* buf, u32 len, u32 off, u32* lo, u32* hi )
{
    if ( off >= len ) { *lo = *hi = len; return; }
    int cls = char_class( (u8)buf[ off ] );
    u32 a = off, b = off;
    while ( a > 0   && char_class( (u8)buf[ a - 1u ] ) == cls ) --a;
    while ( b < len && char_class( (u8)buf[ b ]      ) == cls ) ++b;
    *lo = a; *hi = b;
}

/*----------------------------------------------------------------------------------------------
    input_field_edit -- generic single-line text editing inside a caller-supplied rect.

    Handles: cursor movement (Left / Right / Home / End), selection (Shift variants of the
    above, Ctrl+A), deletion (Backspace / Delete at the caret or over a selection), character
    insertion or selection replacement, horizontal scroll to keep the caret in view, and
    rendering (text content, selection highlight, blinking caret) -- all clipped to the box.

    The caller is responsible for:
        - carving `box` from the layout (widget_next_rect has already been called),
        - drawing the box background and border (so the visual treatment is widget-specific),
        - obtaining `st` from widget_behavior with WIDGET_KIND_FOCUSABLE.
    On Enter or Escape the function drops focus by clearing s_ctx.focused_id.

    Mouse capture: widget_behavior already claims active_id on the press (for every widget
    kind), and that single mechanism is the engine's general-purpose mouse grab -- while a
    widget owns active_id every other widget is frozen (can_hover is false for them) and no
    hover fires elsewhere, so a drag stays bound to this field until the button is released.
    This function leans on exactly that: it tracks the selection drag from st.active and never
    re-tests whether the cursor is still inside the box, so the drag survives the cursor
    leaving the field -- identical in spirit to how the scrollbar knob drags.

    id   -- widget id; keys the persisted imgui_edit_state_t (cursor, anchor, scroll, blink).
    box  -- pixel rect the text renders into; text is inset by WIDGET_PAD on left / right.
    st   -- interaction state from widget_behavior: focused gates keyboard input, pressed marks
            the grab frame, active is held true for the life of the mouse-capture drag.
    buf  -- caller-owned NUL-terminated buffer, modified in-place by keyboard input.
    bufsz-- total byte capacity of buf, including the NUL terminator.

    Returns { .changed = true } on any buffer modification this frame, { .enter = true } when
    Enter is pressed.
----------------------------------------------------------------------------------------------*/

static input_field_result_t
input_field_edit( imgui_id_t id, imgui_rect_t box, widget_state_t st, char* buf, u32 bufsz )
{
    imgui_edit_state_t*  es      = IMGUI_STATE( imgui_edit_state_t, id );
    input_field_result_t res     = { false, false };
    bool                 focused = st.focused;

    u32 len = 0;
    while ( len < bufsz - 1u && buf[ len ] ) ++len;

    /* Clamp cursor and anchor to the current length -- a programmatic buffer change between
       frames may have shortened the string under the old positions. */
    if ( es->cursor > len ) es->cursor = len;
    if ( es->anchor > len ) es->anchor = len;

    u32  sel_lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
    u32  sel_hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
    bool has_sel = ( sel_lo != sel_hi );

    if ( focused )
    {
        bool shift = s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ];
        bool ctrl  = s_io.keys_down[ APP_KEY_LCTRL  ] || s_io.keys_down[ APP_KEY_RCTRL  ];
        bool blink_reset = false;

        /* Clipboard.  Copy / cut are key-driven (only this field knows the selection) and push
           to the OS clipboard via imgui_clipboard_set.  Paste is event-driven: the platform
           already read the OS clipboard on the paste gesture and delivered it in s_io.paste, so
           there is no Ctrl+V key check here -- a non-empty s_io.paste IS the paste.  Resolved
           first so it acts on the selection as the user sees it, before navigation moves it. */

        if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_C ] )
        {
            imgui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
            blink_reset = true;
        }

        if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_X ] )
        {
            imgui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
            memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
            len -= ( sel_hi - sel_lo );
            es->cursor = es->anchor = sel_lo;
            has_sel = false; sel_lo = sel_hi = es->cursor;
            res.changed = true;
            blink_reset = true;
        }

        if ( s_io.paste[ 0 ] )
        {
            /* Drop the selection first so the paste lands where it was. */
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = sel_lo;
                has_sel = false; sel_lo = sel_hi = es->cursor;
            }
            /* Insert each pasted byte at the advancing caret, skipping control characters
               (a single-line field rejects newlines / tabs) and stopping at capacity. */
            for ( const char* c = s_io.paste; *c && len + 1u < bufsz; ++c )
            {
                if ( (u8)*c < 0x20u || (u8)*c == 0x7Fu ) continue;
                memmove( buf + es->cursor + 1u, buf + es->cursor, len - es->cursor + 1u );
                buf[ es->cursor ] = *c;
                ++len; ++es->cursor;
            }
            es->anchor  = es->cursor;
            res.changed = true;
            blink_reset = true;
        }

        /* Navigation: Left / Right collapse or extend the selection; Home / End jump. */

        /* Navigation + deletion read keys_pressed_repeat so a held key repeats at the OS rate;
           submit / escape / select-all read the initial-press snapshot (they must fire once). */
        if ( s_io.keys_pressed_repeat[ APP_KEY_LEFT ] )
        {
            if ( !shift && has_sel ) { es->cursor = es->anchor = sel_lo; }
            else if ( es->cursor > 0 ) { --es->cursor; if ( !shift ) es->anchor = es->cursor; }
            blink_reset = true;
        }

        if ( s_io.keys_pressed_repeat[ APP_KEY_RIGHT ] )
        {
            if ( !shift && has_sel ) { es->cursor = es->anchor = sel_hi; }
            else if ( es->cursor < len ) { ++es->cursor; if ( !shift ) es->anchor = es->cursor; }
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_HOME ] )
        {
            es->cursor = 0; if ( !shift ) es->anchor = 0;
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_END ] )
        {
            es->cursor = len; if ( !shift ) es->anchor = len;
            blink_reset = true;
        }

        /* Ctrl+A: select the entire buffer. */
        if ( ctrl && s_io.keys_pressed[ APP_KEY_A ] )
        {
            es->anchor = 0; es->cursor = len;
            blink_reset = true;
        }

        /* Backspace: delete the selection, or the character before the caret. */
        if ( s_io.keys_pressed_repeat[ APP_KEY_BACKSPACE ] )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = sel_lo;
                res.changed = true;
            }
            else if ( es->cursor > 0 )
            {
                --es->cursor;
                memmove( buf + es->cursor, buf + es->cursor + 1u, len - es->cursor );
                --len; buf[ len ] = '\0';
                es->anchor = es->cursor;
                res.changed = true;
            }
            has_sel = false; sel_lo = sel_hi = es->cursor;
            blink_reset = true;
        }

        /* Delete: delete the selection, or the character after the caret. */
        if ( s_io.keys_pressed_repeat[ APP_KEY_DELETE ] )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = sel_lo;
                res.changed = true;
            }
            else if ( es->cursor < len )
            {
                memmove( buf + es->cursor, buf + es->cursor + 1u, len - es->cursor );
                --len; buf[ len ] = '\0';
                res.changed = true;
            }
            has_sel = false; sel_lo = sel_hi = es->cursor;
            blink_reset = true;
        }

        /* Character input: replace the selection with the first incoming char, then insert
           any remaining chars at the advancing caret.  Selection is cleared after the first
           replacement so subsequent chars in the same frame insert normally.  Skipped while
           Ctrl is held so shortcut combos (Ctrl+C/V/X/A) never leak a stray glyph -- the OS
           filters their control codes too, but this keeps the contract explicit. */
        for ( const char* ch = ctrl ? "" : s_io.text; *ch; ++ch )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo + 1u, buf + sel_hi, len - sel_hi + 1u );
                buf[ sel_lo ] = *ch;
                len          -= ( sel_hi - sel_lo ) - 1u;
                es->cursor    = es->anchor = sel_lo + 1u;
                has_sel       = false; sel_lo = sel_hi = es->cursor;
            }
            else if ( len + 1u < bufsz )
            {
                memmove( buf + es->cursor + 1u, buf + es->cursor, len - es->cursor + 1u );
                buf[ es->cursor ] = *ch;
                ++len; ++es->cursor;
                es->anchor = es->cursor;
            }
            res.changed = true;
            blink_reset = true;
        }

        /* Enter submits; Escape cancels; both drop focus. */
        if ( s_io.keys_pressed[ APP_KEY_ENTER ] )
        {
            s_ctx.focused_id = IMGUI_ID_NONE;
            res.enter = true;
        }
        if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
            s_ctx.focused_id = IMGUI_ID_NONE;

        /* Mouse.  st.pressed is the grab frame (also the focus-gaining click, since
           widget_behavior set focused_id = id by now); st.active stays true for the whole
           capture, so the drag below keeps extending the selection even after the cursor
           leaves the box.  text_offset_at clamps a cursor past either edge to 0 / len, so a
           drag past the ends selects to the start / end naturally. */
        {
            f32 px  = s_io.mouse_x - ( box.x + WIDGET_PAD ) + es->scroll_x;
            u32 off = text_offset_at( buf, len, px );

            if ( st.pressed && s_io.mouse_double[ 0 ] )
            {
                /* Double-click: snap the selection to the word under the cursor. */
                word_bounds( buf, len, off, &es->anchor, &es->cursor );
                blink_reset = true;
            }
            else if ( st.pressed )
            {
                /* Single press: caret to the click; Shift keeps the anchor to extend. */
                es->cursor = off;
                if ( !shift ) es->anchor = off;
                blink_reset = true;
            }
            else if ( st.active )
            {
                /* Drag: move the caret, leaving the anchor put, so the selection grows. */
                es->cursor  = off;
                blink_reset = true;
            }
        }

        /* Recompute selection bounds after all edits this frame. */
        sel_lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
        sel_hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
        has_sel = ( sel_lo != sel_hi );

        if ( blink_reset ) es->blink_t = 0.0f;
        else               es->blink_t += s_io.dt;
    }

    /* Scroll to keep the caret inside the visible width on every frame, not just when
       focused, so a programmatic cursor move from outside is also honoured. */
    {
        f32 cx    = text_x_at( buf, es->cursor );
        f32 vis_w = box.w - 2.0f * WIDGET_PAD;
        if ( vis_w < 0.0f ) vis_w = 0.0f;
        if ( cx - es->scroll_x < 0.0f )  es->scroll_x = cx;
        if ( cx - es->scroll_x > vis_w ) es->scroll_x = cx - vis_w;
        if ( es->scroll_x < 0.0f )       es->scroll_x = 0.0f;
    }

    /* Clip text, selection, and caret to the box interior so scrolled content does not
       bleed past the border.  Balanced with draw_pop_clip_rect below. */
    draw_push_clip_rect( box.x + WIN_BORDER, box.y + WIN_BORDER,
                         box.w - 2.0f * WIN_BORDER, box.h - 2.0f * WIN_BORDER );

    f32 text_x = box.x + WIDGET_PAD - es->scroll_x;
    f32 text_y = text_center_y( box.y, box.h );

    /* Selection highlight behind the text. */
    if ( focused && has_sel )
    {
        f32 sx0 = text_x + text_x_at( buf, sel_lo );
        f32 sx1 = text_x + text_x_at( buf, sel_hi );
        draw_push_rect_filled( sx0, box.y + 1.0f, sx1 - sx0, box.h - 2.0f,
                               0, 0, 1, 1, 0, COL_WIDGET_ACT );
    }

    draw_push_text( text_x, text_y, COL_TEXT, buf );

    /* Blinking caret: visible for the first 0.5 s of each 1 s cycle. */
    if ( focused )
    {
        bool caret_vis = ( ( (u32)( es->blink_t * 2.0f ) ) & 1u ) == 0u;
        if ( caret_vis )
        {
            f32 cx = text_x + text_x_at( buf, es->cursor );
            draw_push_rect_filled( cx, box.y + (f32)s_layout.cursor_inset,
                                   (f32)s_layout.cursor_w,
                                   box.h - 2.0f * (f32)s_layout.cursor_inset,
                                   0, 0, 1, 1, 0, COL_CURSOR );
        }
    }

    draw_pop_clip_rect();

    return res;
}

// clang-format on
/*============================================================================================*/
