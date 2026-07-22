/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_text_edit.c -- single-line text field widget.

    The visual + measurement half of input_text.  It carves the box, measures glyph advances to
    place the caret and map clicks to byte offsets, tracks the mouse selection drag, scrolls
    horizontally to keep the caret in view, and renders text / selection / blinking caret -- all
    clipped to a caller-supplied box.  The keyboard editing ENGINE (buffer mutation, cursor +
    selection, word motion, undo / redo, clipboard, key -> command) lives one layer down as a
    gesture mechanism -- interact/gui_edit.c -- driven here through edit_keys().  input_text
    (gui_input.c) is a thinner wrapper still: it owns the label split, box chrome, and focus claim.

    The persisted per-id edit state (gui_edit_state_t) lives in the keyed state pool; this widget
    allocates it and hands the engine a pointer.  The pure byte-offset helpers (char_class,
    word_bounds, word_click_off, edit_strlen, edit_sel) come from the engine seam
    (interact/gui_interact.h); text_x_at / text_offset_at (font measurement) are defined here and
    shared with the multiline wrapper (gui_text_edit_multi.c).

    Part of the chrome unit; gui_item_state_t, text_center_y, the COL_* palette, WIDGET_PAD /
    WIN_BORDER, and the draw + font helpers are all in scope.

==============================================================================================*/
// clang-format off

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

/* Mouse handling for a focused field: click-to-caret, Shift-extend, double-click word select,
   and the click-drag that extends the selection (plain, or by whole words after a double-click).
   Runs off st.active so the drag survives the cursor leaving the box, like the scrollbar knob.
   Needs measurement (text_offset_at) to turn the cursor into a byte offset, then applies the
   engine's word helpers (word_bounds / word_click_off) -- so it stays widget-side. */
static void
edit_apply_mouse( gui_rect_t box, gui_item_state_t st, char* buf, u32 len,
                  gui_edit_state_t* es, bool shift, bool* blink_io )
{
    bool blink_reset = *blink_io;

    /* st.pressed is the grab frame (also the focus-gaining click, since item_state set
       focused_id = id by now); st.active stays true for the whole capture, so the drag below
       keeps extending the selection even after the cursor leaves the box.  text_offset_at clamps
       a cursor past either edge to 0 / len, so a drag past the ends selects to start / end. */

    f32 px  = s_io.mouse_x - ( box.x + WIDGET_PAD ) + es->scroll_x;
    u32 off = text_offset_at( buf, len, px );

    if ( st.pressed && s_io.mouse_double[ 0 ] )
    {
        /* Double-click: select the word under the cursor. */
        u32 wb_off = word_click_off( buf, len, off );
        u32 wlo, whi;
        word_bounds( buf, len, wb_off, &wlo, &whi );
        es->anchor   = (u16)wlo;
        es->cursor   = (u16)whi;
        es->dbl_lo   = (u16)wlo;
        es->dbl_hi   = (u16)whi;
        es->word_sel = 1;
        blink_reset  = true;
    }
    else if ( st.pressed )
    {
        /* Single press: caret to the click; Shift keeps the anchor to extend. */
        es->cursor   = (u16)off;
        es->word_sel = 0;
        if ( !shift ) es->anchor = (u16)off;
        blink_reset = true;
    }
    else if ( st.active )
    {
        if ( es->word_sel )
        {
            /* Word-select drag: keep the initial double-clicked word selected and extend
               by word boundaries when the mouse moves outside it.
               Apply the same right-edge correction as the double-click itself. */
            u32 drag_off = word_click_off( buf, len, off );
            if ( drag_off < es->dbl_lo )
            {
                /* Dragged left of original word: pin right at dbl_hi, extend left. */
                u32 wlo, whi;
                word_bounds( buf, len, drag_off, &wlo, &whi );
                es->anchor = es->dbl_hi;
                es->cursor = (u16)wlo;
            }
            else if ( drag_off >= es->dbl_hi )
            {
                /* Dragged right of original word: pin left at dbl_lo, extend right. */
                u32 wlo, whi;
                word_bounds( buf, len, drag_off, &wlo, &whi );
                es->anchor = es->dbl_lo;
                es->cursor = (u16)whi;
            }
            else
            {
                /* Still inside the original word: restore the initial word selection. */
                es->anchor = es->dbl_lo;
                es->cursor = es->dbl_hi;
            }
        }
        else
        {
            /* Normal drag: move the caret, leaving the anchor put. */
            es->cursor = (u16)off;
        }
        blink_reset = true;
    }

    *blink_io = blink_reset;
}

/* Horizontal scroll + paint: keep the caret in view, then draw the selection highlight, the
   glyph-clipped text, and the blinking caret -- all inside the box interior.  Runs every frame
   (focused or not) so a programmatic caret move from outside is honoured and the field always
   repaints its content. */

static void
edit_scroll_and_paint( gui_rect_t box, char* buf, gui_edit_state_t* es, bool focused )
{
    /* Scroll to keep the caret inside the visible width on every frame, not just when
       focused, so a programmatic cursor move from outside is also honoured. */
    {
        f32  cx    = text_x_at( buf, es->cursor );
        f32  vis_w = box.w - 2.0f * WIDGET_PAD;
        if ( vis_w < 0.0f ) vis_w = 0.0f;
        if ( cx - (f32)es->scroll_x < 0.0f )  es->scroll_x = (u16)cx;
        if ( cx - (f32)es->scroll_x > vis_w ) es->scroll_x = (u16)( cx - vis_w );
    }

    u32  sel_lo, sel_hi;
    bool has_sel;
    edit_sel( es, &sel_lo, &sel_hi, &has_sel );

    /* Clip text, selection, and caret to the box interior so scrolled content does not bleed past
       the border.  Glyph-level horizontal clip: the scrolled text is hard-cut to the [clip_x0,
       clip_x1] window at emit time (straddling glyphs sliced with remapped U), so no scissor / no
       draw-call split -- the field stays merged into the surrounding window batch even when scrolled
       (the self-fit-over-clips rule, now self-fitting at the glyph rather than the label boundary).
       The selection rect is clamped to the same window by hand; the caret is kept inside the window
       by the scroll math above so it never needs clipping. */
    f32 text_x = box.x + WIDGET_PAD - es->scroll_x;
    f32 text_y = text_center_y( box.y, box.h );
    f32 clip_x0 = box.x + WIDGET_PAD;
    f32 clip_x1 = box.x + box.w - WIDGET_PAD;

    /* Selection highlight behind the text, clamped to the visible window. */
    if ( focused && has_sel )
    {
        f32 sx0 = text_x + text_x_at( buf, sel_lo );
        f32 sx1 = text_x + text_x_at( buf, sel_hi );
        if ( sx0 < clip_x0 ) sx0 = clip_x0;
        if ( sx1 > clip_x1 ) sx1 = clip_x1;
        if ( sx1 > sx0 )
            draw_fill( ( gui_rect_t ){ sx0, box.y + 1.0f, sx1 - sx0, box.h - 2.0f },
                       COL_WIDGET_ACT );
    }

    draw_push_text_clip_n( text_x, text_y, COL_TEXT, buf, 0xFFFFFFFFu, clip_x0, clip_x1 );

    /* Blinking caret: visible for the first 0.5 s of each 1 s cycle. */
    bool enable_caret_blink = true;
    if ( focused )
    {
        bool in_visible_half_of_blink_cycle = ( ( (u32)( es->blink_t * 2.0f ) ) & 1u ) == 0u;
        bool caret_vis = !enable_caret_blink || in_visible_half_of_blink_cycle;
        if ( caret_vis )
        {
            f32 cx = text_x + text_x_at( buf, es->cursor );
            draw_fill( ( gui_rect_t ){ cx, box.y + (f32)s_style.cursor_inset,
                                       (f32)s_style.cursor_w,
                                       box.h - 2.0f * (f32)s_style.cursor_inset },
                       COL_CURSOR );
        }
    }
}

/*==============================================================================================
    input_field_edit -- single-line text field over a caller-supplied rect.

    The widget half: it drives the interact edit engine (edit_keys) for all keyboard behavior,
    then handles the mouse selection drag (which needs measurement) and paints the result.

    The caller is responsible for:
        - carving `box` from the layout (cell_next has already been called),
        - drawing the box background and border (so the visual treatment is widget-specific),
        - obtaining `st` from item_state with ITEM_FOCUSABLE.

    Mouse capture: item_state already claims active_id on the press, and while a widget owns
    active_id every other widget is frozen -- so a selection drag stays bound to this field until
    release, identical in spirit to the scrollbar knob.

    id          -- widget id; keys the persisted gui_edit_state_t (cursor, anchor, scroll, blink).
    box         -- pixel rect the text renders into; text is inset by WIDGET_PAD on left / right.
    st          -- interaction state from item_state: focused gates keyboard input, pressed marks
                   the grab frame, active is held for the life of the mouse-capture drag.
    buf / bufsz -- caller-owned NUL-terminated buffer (modified in-place) and its capacity.
    on_change   -- optional callback fired after any frame that modifies the buffer.
    cb_user     -- opaque pointer forwarded verbatim to on_change.

    Returns { .changed = true } on any buffer modification, { .enter = true } on Enter.
==============================================================================================*/
static input_field_result_t
input_field_edit( gui_id_t id, gui_rect_t box, gui_item_state_t st, char* buf, u32 bufsz,
                  gui_text_cb_fn on_change, void* cb_user )
{
    gui_edit_state_t*    es      = GUI_STATE( gui_edit_state_t, id );
    input_field_result_t res     = { false, false };
    bool                 focused = st.focused;

    /* I-beam over a text field -- and held through a selection drag (st.active), so it does not
       flip back to the arrow while the cursor sweeps outside the box mid-drag. */
    if ( st.hover || st.active )
        cursor_set( APP_CURSOR_TEXT );

    u32 len = edit_strlen( buf, bufsz );

    /* Clamp cursor and anchor to the current length -- a programmatic buffer change between
       frames may have shortened the string under the old positions. */
    if ( es->cursor > len ) es->cursor = (u16)len;
    if ( es->anchor > len ) es->anchor = (u16)len;

    if ( focused )
    {
        bool blink_reset = false;

        /* The interact edit engine owns the entire keyboard path (key hook, cursor-end request,
           undo ring, selection publish, and every key command); this widget owns measurement,
           the mouse selection drag, and paint. */

        res = edit_keys( id, buf, bufsz, es, &blink_reset );
        len = edit_strlen( buf, bufsz );   /* keys may have resized buf under us */

        edit_apply_mouse( box, st, buf, len, es, io_shift(), &blink_reset );

        if ( blink_reset ) es->blink_t = 0.0f;
        else               es->blink_t += s_io.dt;
    }

    edit_scroll_and_paint( box, buf, es, focused );

    /* Fire the change callback after all rendering so the caller sees the final state. */
    if ( res.changed && on_change )
    {
        u32 final_len = edit_strlen( buf, bufsz );
        on_change( buf, final_len, bufsz, cb_user );
    }

    /* Accumulate the edit flag for is_item_deactivated_after_edit (core/gui_query.c). */
    if ( res.changed )
        item_mark_edited();

    return res;
}

/*==============================================================================================
    num_edit_field -- shared numeric text-entry over a caller-drawn box.

    The seed / edit / parse cycle every numeric text field runs, factored here (above both its
    users in the unity include order) so there is one implementation: the numeric inputs
    (input_int / input_float / input_double, gui_widget_numeric.c) and a drag / slider box switched
    into text entry by a Ctrl+Click (gui_widget_slider.c) share it.

      - Focus gain (st.focused, scratch not yet ours): seed the scratch from cur via seed_fmt.
      - Focused: run input_field_edit on the scratch; on Enter, parse it back with strtod (decimal
        or scientific, e.g. "1e+8") and commit if it differs from cur.
      - Focus loss with the scratch still ours: parse and commit the same way.

    One scratch slot keyed by id -- only one field is focused at a time, the same single-owner
    reason the undo ring is one global.  seed_fmt is the format used ONLY to seed the editable text
    (input_* pass their display format; a drag box passes a decoration-free "%d" / "%g" so a
    captioned display like "HP: %d" does not seed an unparseable buffer).  is_int casts through int
    so "%d" seeding does not UB.  Returns true and writes *out only on a committed change; the
    caller owns the box frame draw and the not-focused static value display.
==============================================================================================*/

#define GUI_NUM_EDIT_CAP 64

static char     s_num_edit_buf[ GUI_NUM_EDIT_CAP ];
static gui_id_t s_num_edit_id = GUI_ID_NONE;

/* True while `id` owns the numeric scratch: focused and typing, or the one blur frame still
   pending its parse.  A caller (a drag box) uses it to stay in text-entry presentation until the
   commit lands, one frame after focus is lost. */
static bool
num_edit_active( gui_id_t id )
{
    return s_num_edit_id == id;
}

static bool
num_edit_field( gui_id_t id, gui_rect_t box_r, gui_item_state_t st,
                const char* seed_fmt, bool is_int, double cur, double* out )
{
    bool committed = false;

    /* Focus gain: seed the scratch with the current value in the (decoration-free) seed format. */
    if ( st.focused && s_num_edit_id != id )
    {
        if ( is_int ) fmt_snprintf( s_num_edit_buf, GUI_NUM_EDIT_CAP, seed_fmt, (int)cur );
        else          fmt_snprintf( s_num_edit_buf, GUI_NUM_EDIT_CAP, seed_fmt, cur );
        s_num_edit_id = id;
    }

    if ( st.focused )
    {
        input_field_result_t res =
            input_field_edit( id, box_r, st, s_num_edit_buf, GUI_NUM_EDIT_CAP, NULL, NULL );
        if ( res.enter )
        {
            double parsed = strtod( s_num_edit_buf, NULL );
            if ( parsed != cur ) { *out = parsed; committed = true; }
            s_num_edit_id = GUI_ID_NONE;
        }
    }
    else if ( s_num_edit_id == id )
    {
        /* Focus loss: the scratch was ours -- parse and commit. */
        double parsed = strtod( s_num_edit_buf, NULL );
        if ( parsed != cur ) { *out = parsed; committed = true; }
        s_num_edit_id = GUI_ID_NONE;
    }

    return committed;
}

// clang-format on
/*============================================================================================*/
