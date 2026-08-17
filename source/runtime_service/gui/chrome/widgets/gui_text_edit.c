/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_text_edit.c -- single-line text field widget.

    The paint half of input_text, and nothing more.  The entire behavior of the field -- buffer
    editing, cursor + selection, word motion, undo / redo, clipboard, glyph measurement, the mouse
    selection drag, and horizontal scroll -- lives one layer down as an interact mechanism
    (interact/gui_edit.c), driven here through the single call edit_field().  This widget only
    insets the caller's box to a content rect, hands it to the engine, and then paints the state
    the engine resolved (text / selection highlight / blinking caret), clipped to the box interior.
    input_text (gui_input.c) is a thinner wrapper still: it owns the label split, box chrome, and
    focus claim.

    The measurement helpers (text_x_at) and the byte-offset helpers (edit_sel) used by the paint
    come from the engine seam (interact/gui_interact.h) and are shared with the multiline wrapper
    (gui_text_edit_multi.c).

    Part of the chrome unit; gui_item_state_t, text_center_y, the COL_* palette, WIDGET_PAD, and
    the draw + font helpers are all in scope.

==============================================================================================*/
// clang-format off

/* The text-selection band's colour, and the one spelling of it for both editors.

   BG washed for being chosen -- "a control surface, chosen, at rest" -- which is the same
   reasoning gui_select.c's marquee runs on, one role down because a field's ground is a control
   face and not a container.  It used to reach for COL_BG_ACTIVE, the PRESSED control face, which
   was invisible for exactly the reason a pressed face is the wrong cell: the field itself was
   painted with BG/ACTIVE while focused, and a selection can only ever be drawn while focused,
   so highlight and ground were the same colour by construction.  Fields rest on BG/IDLE now
   (input_text_begin), so the band reads against the ground it is actually drawn on.

   No COL_* macro: the colour macros spell (role, phase) plain, deliberately (see
   style/gui_style.h) -- a selected read is named at the read site. */
static u32
edit_sel_color( void )
{
    return style_col_selected( GUI_ROLE_BG, GUI_PHASE_IDLE );
}

/* The selection band for one text run: the GLYPH BOX of the line, bled a pixel each way, held
   inside `bound`.  Anchored to the text's own y and char_h rather than to the widget's row,
   because those two are independent -- the row is a style metric and the glyph box is a font
   metric, so a band cut from the row sits centred on the row and NOT on the ink whenever the
   font is shorter than the row (the extra height reads as a low-hanging fill, since latin ink
   crowds the top of the box and leaves the descender band empty).  Same anchor the multiline
   editor and the window-level select highlight use. */
static gui_rect_t
edit_sel_band( f32 x0, f32 x1, f32 text_y, gui_rect_t bound )
{
    f32 y = text_y - 1.0f;
    f32 h = font_char_h() + 2.0f;

    if ( y < bound.y ) { h -= bound.y - y; y = bound.y; }
    if ( y + h > bound.y + bound.h ) h = bound.y + bound.h - y;

    return ( gui_rect_t ){ x0, y, x1 - x0, h };
}

/* Paint a focused-or-not field over its content rect: the selection highlight behind the text,
   the glyph-clipped text, and the blinking caret -- all inside the content interior so scrolled
   content does not bleed past the border.  Reads the state the engine left on the slot
   (cursor / anchor / pan_x / blink_t); measures with the engine's text_x_at.  Runs every frame
   so a programmatic caret move from outside is honoured and the field always repaints its content.

   Glyph-level horizontal clip: the scrolled text is hard-cut to the [clip_x0, clip_x1] window at
   emit time (straddling glyphs sliced with remapped U), so no scissor / no draw-call split -- the
   field stays merged into the surrounding window batch even when scrolled (the self-fit-over-clips
   rule, self-fitting at the glyph rather than the label boundary).  The selection rect is clamped
   to the same window by hand; the caret is kept inside the window by the engine's scroll math so
   it never needs clipping. */
static void
edit_paint( gui_rect_t content, const char* buf, const gui_edit_state_t* es, bool focused )
{
    u32  sel_lo, sel_hi;
    bool has_sel;
    edit_sel( es, &sel_lo, &sel_hi, &has_sel );

    f32 text_x  = content.x - es->pan_x;
    f32 text_y  = text_center_y( content.y, content.h );
    f32 clip_x0 = content.x;
    f32 clip_x1 = content.x + content.w;

    /* Selection highlight behind the text, clamped to the visible window. */
    if ( focused && has_sel )
    {
        f32 sx0 = text_x + text_x_at( buf, sel_lo );
        f32 sx1 = text_x + text_x_at( buf, sel_hi );
        if ( sx0 < clip_x0 ) sx0 = clip_x0;
        if ( sx1 > clip_x1 ) sx1 = clip_x1;
        if ( sx1 > sx0 )
            draw_fill( edit_sel_band( sx0, sx1, text_y, content ), edit_sel_color() );
    }

    draw_push_text_clip_n( text_x, text_y, COL_TEXT_PRIMARY_IDLE, buf, 0xFFFFFFFFu, clip_x0, clip_x1 );

    /* Blinking caret: visible for the first 0.5 s of each 1 s cycle. */
    if ( focused )
    {
        bool in_visible_half_of_blink_cycle = ( ( (u32)( es->blink_t * 2.0f ) ) & 1u ) == 0u;
        if ( in_visible_half_of_blink_cycle )
        {
            /* The caret box is the GLYPH box -- the same anchor the selection band uses, for the
               same reason (see edit_sel_band): the row is a style metric and the glyph box a font
               metric.  Deriving the caret from the row minus WIDGET_PAD collapsed it to a sliver
               whenever a density pass widened the pad past the font's headroom. */
            f32 cx = text_x + text_x_at( buf, es->cursor );
            /* Caret width tracks the frame weight but floors at 1 px: a caret is ink the user is
               staring at, and a border-0 theme must not blank it. */
            f32 cw = WIN_BORDER;  if ( cw < 1.0f ) cw = 1.0f;
            draw_fill( edit_sel_band( cx, cx + cw, text_y, content ), COL_TEXT_PRIMARY_IDLE );
        }
    }
}

/*==============================================================================================
    input_field_edit -- single-line text field over a caller-supplied rect.

    The paint half: it insets the box to the text content rect, drives the interact edit engine
    (edit_field) for ALL behavior -- keys, mouse selection drag, scroll, undo, blink -- then paints
    the resolved state.  The engine owns the field's persisted edit state and every mutation; this
    widget adds only the visual treatment and the on_change callback.

    The caller is responsible for:
        - carving `box` from the layout (cell_next has already been called),
        - drawing the box background and border (so the visual treatment is widget-specific),
        - obtaining `st` from item_state with ITEM_FOCUSABLE.

    Mouse capture: item_state already claims active_id on the press, and while a widget owns
    active_id every other widget is frozen -- so the engine's selection drag stays bound to this
    field until release, identical in spirit to the scrollbar knob.

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
    /* Content rect: the box inset by WIDGET_PAD on left / right (the engine works in this space,
       so it never sees the widget's padding); vertical extent unchanged for centering + caret. */
    gui_rect_t content = { box.x + WIDGET_PAD, box.y, box.w - 2.0f * WIDGET_PAD, box.h };

    /* The engine runs the whole non-paint frame (keys, mouse, pan, blink, undo) and leaves the
       resolved cursor / anchor / pan_x / blink_t on the keyed edit-state slot. */
    input_field_result_t res = edit_field( id, content, st, buf, bufsz );
    edit_filter_set( GUI_INPUT_FILTER_NONE );   /* the ambient filter is per-run; never leaks */

    edit_paint( content, buf, GUI_STATE( gui_edit_state_t, id ), st.focused );

    /* Fire the change callback after all rendering so the caller sees the final state. */
    if ( res.changed && on_change )
    {
        u32 final_len = edit_strlen( buf, bufsz );
        on_change( buf, final_len, bufsz, cb_user );
    }

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
        /* A numeric scratch accepts only its own vocabulary (strtod's: decimal or scientific). */
        edit_filter_set( is_int ? GUI_INPUT_FILTER_INT : GUI_INPUT_FILTER_DECIMAL );
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
