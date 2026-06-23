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
    - Ctrl+Left/Right to word-jump; Shift+Ctrl+Left/Right extends the selection by word
    - Home/End
    - Shift+any of the above to extend selection
    - Ctrl+A to select all
    - Ctrl+Z to undo; Ctrl+Y / Ctrl+Shift+Z to redo
    - Ctrl+C / Ctrl+X copy / cut to the OS clipboard; paste applied from s_io.paste (the
      platform posts APP_EV_CLIPBOARD on the paste gesture; no Ctrl+V check needed here)
    - Backspace and Delete both at caret and over selection
    - Character insertion at caret, or selection replacement on first char
    - Blinking caret (reset to visible on any keypress or click)
    - Click-to-caret positioning (also fires on the focus-gaining click)
    - Click-drag to select, double-click to select the word under the cursor
    - Horizontal scroll that keeps the caret visible whenever it moves
    - Draw clip so scrolled text never bleeds past the box border
    - Escape reverts the buffer to its content at focus-gain time

----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32  blink_t;   /* seconds since last caret-visibility reset (fractional dt accumulator) */
    u16  cursor;    /* byte offset of the caret (insertion / deletion point) */
    u16  anchor;    /* passive end of the selection; cursor == anchor -> none */
    u16  dbl_lo;    /* word start of the double-clicked word (word-drag mode) */
    u16  dbl_hi;    /* word end of the double-clicked word  (word-drag mode)  */
    u16  scroll_x;  /* horizontal scroll offset in pixels (integer: sum of integer glyph advances) */
    u8   word_sel;  /* nonzero while in word-select drag (set by double-click) */
    u8   _pad;

} imgui_edit_state_t;
/* 16 bytes -- fits within IMGUI_STATE_CAP. */

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
    Undo / redo ring buffer -- single global, keyed to the focused widget.

    Only one text field can hold focus at a time, so a single ring buffer keyed by focused_id
    is sufficient.  The ring stores up to UNDO_SLOTS snapshots of the full buffer content plus
    cursor/anchor; each snapshot fits in UNDO_TEXT_MAX bytes.

    Snapshots are pushed AFTER each committed edit.  Cursor and anchor stored in the snapshot
    represent the state immediately after that edit, so restoring a snapshot also restores
    the caret to a sensible position.

    Grouping: consecutive single-character insertions are merged into one undo step (the
    snapshot for the current group is updated in place) so a long typing burst undoes as a
    whole word rather than character by character.  Any other edit (paste, delete, cut) breaks
    the group and starts a fresh snapshot.

    Escape-to-revert uses a separate `revert` copy saved at focus-gain, which survives ring
    wrapping and is always the unmodified field content.
----------------------------------------------------------------------------------------------*/

#define UNDO_SLOTS    16
#define UNDO_TEXT_MAX 256

typedef struct
{
    char text[ UNDO_TEXT_MAX ];
    u16  cursor;
    u16  anchor;
} imgui_undo_snap_t;

typedef struct
{
    imgui_id_t        for_id;                   // which widget owns this history
    char              revert[ UNDO_TEXT_MAX ];  // buffer content at focus-gain
    imgui_undo_snap_t ring[ UNDO_SLOTS ];       // circular snapshot ring
    i32               base;                     // ring index of logical slot 0
    i32               cur;                      // logical index of the current (live) snapshot
    i32               top;                      // one past the highest committed logical index
    bool              last_was_char;            // true when the last push was a char insert

} imgui_undo_buf_t;

static imgui_undo_buf_t s_undo;

/* Copy a NUL-terminated string into a fixed UNDO_TEXT_MAX buffer safely. */
static void
undo_text_copy( char* dst, const char* src )
{
    u32 n = 0;
    while ( n < UNDO_TEXT_MAX - 1u && src[ n ] ) ++n;
    memcpy( dst, src, n );
    dst[ n ] = '\0';
}

/* Advance the ring head and write a new snapshot.  Drops the oldest entry when the ring is
   full.  Clears redo history (top = cur + 1).  Resets last_was_char. */
static void
undo_push( imgui_undo_buf_t* u, const char* text, u16 cursor, u16 anchor )
{
    u->cur++;
    if ( u->cur == UNDO_SLOTS )
    {
        u->base = ( u->base + 1 ) % UNDO_SLOTS;
        u->cur  = UNDO_SLOTS - 1;
    }
    imgui_undo_snap_t* s = &u->ring[ ( u->base + u->cur ) % UNDO_SLOTS ];
    undo_text_copy( s->text, text );
    s->cursor        = cursor;
    s->anchor        = anchor;
    u->top           = u->cur + 1;
    u->last_was_char = false;
}

/* Overwrite the current snapshot in place (char-insert grouping: extend the current word
   without creating a new undo step). */
static void
undo_update( imgui_undo_buf_t* u, const char* text, u16 cursor, u16 anchor )
{
    imgui_undo_snap_t* s = &u->ring[ ( u->base + u->cur ) % UNDO_SLOTS ];
    undo_text_copy( s->text, text );
    s->cursor = cursor;
    s->anchor = anchor;
}

/* Initialize the undo ring for a newly focused widget.  Saves the revert copy and pushes
   the initial state as the floor of the undo stack. */
static void
undo_init( imgui_undo_buf_t* u, imgui_id_t id, const char* buf, u16 cursor, u16 anchor )
{
    u->for_id        = id;
    u->base          = 0;
    u->cur           = 0;
    u->top           = 1;
    u->last_was_char = false;
    undo_text_copy( u->revert, buf );
    imgui_undo_snap_t* s = &u->ring[ 0 ];
    undo_text_copy( s->text, buf );
    s->cursor = cursor;
    s->anchor = anchor;
}

/* Apply a ring snapshot to buf/es; safe if the snapshot text is longer than bufsz (truncates).
   Returns true so callers can chain: res.changed = undo_apply(...). */
static bool
undo_apply( imgui_undo_buf_t* u, i32 logical_idx, char* buf, u32 bufsz,
            imgui_edit_state_t* es )
{
    imgui_undo_snap_t* s   = &u->ring[ ( u->base + logical_idx ) % UNDO_SLOTS ];
    u32                n   = 0;
    while ( n < bufsz - 1u && s->text[ n ] ) ++n;
    memcpy( buf, s->text, n );
    buf[ n ]   = '\0';
    es->cursor = ( s->cursor <= n ) ? s->cursor : (u16)n;
    es->anchor = ( s->anchor <= n ) ? s->anchor : (u16)n;
    return true;
}

/*----------------------------------------------------------------------------------------------
    input_field_edit -- generic single-line text editing inside a caller-supplied rect.

    Handles: cursor movement (Left / Right / Home / End, Ctrl variants for word jump),
    selection (Shift variants of the above, Ctrl+A), undo / redo (Ctrl+Z / Ctrl+Y /
    Ctrl+Shift+Z), deletion (Backspace / Delete at the caret or over a selection), character
    insertion or selection replacement, horizontal scroll to keep the caret in view, and
    rendering (text content, selection highlight, blinking caret) -- all clipped to the box.

    Escape reverts the buffer to its content at focus-gain time.

    The caller is responsible for:
        - carving `box` from the layout (widget_next_rect has already been called),
        - drawing the box background and border (so the visual treatment is widget-specific),
        - obtaining `st` from widget_behavior with WIDGET_KIND_FOCUSABLE.
    On Enter or Escape the function drops focus by clearing s_interaction.focused_id.

    Mouse capture: widget_behavior already claims active_id on the press (for every widget
    kind), and that single mechanism is the engine's general-purpose mouse grab -- while a
    widget owns active_id every other widget is frozen (can_hover is false for them) and no
    hover fires elsewhere, so a drag stays bound to this field until the button is released.
    This function leans on exactly that: it tracks the selection drag from st.active and never
    re-tests whether the cursor is still inside the box, so the drag survives the cursor
    leaving the field -- identical in spirit to how the scrollbar knob drags.

    id          -- widget id; keys the persisted imgui_edit_state_t (cursor, anchor, scroll, blink).
    box         -- pixel rect the text renders into; text is inset by WIDGET_PAD on left / right.
    st          -- interaction state from widget_behavior: focused gates keyboard input, pressed marks
                   the grab frame, active is held true for the life of the mouse-capture drag.
    buf         -- caller-owned NUL-terminated buffer, modified in-place by keyboard input.
    bufsz       -- total byte capacity of buf, including the NUL terminator.
    on_change   -- optional callback fired after any frame that modifies the buffer; receives the
                   new text, its length, the buffer capacity, and the caller-supplied user pointer.
    cb_user     -- opaque pointer forwarded verbatim to on_change (ignored when on_change is NULL).

    Returns { .changed = true } on any buffer modification this frame, { .enter = true } when
    Enter is pressed.
----------------------------------------------------------------------------------------------*/

static input_field_result_t
input_field_edit( imgui_id_t id, imgui_rect_t box, widget_state_t st, char* buf, u32 bufsz,
                  imgui_text_cb_fn on_change, void* cb_user )
{
    imgui_edit_state_t*  es      = IMGUI_STATE( imgui_edit_state_t, id );
    input_field_result_t res     = { false, false };
    bool                 focused = st.focused;

    /* I-beam over a text field -- and held through a selection drag (st.active), so it does not flip
       back to the arrow while the cursor sweeps outside the box mid-drag. */
    if ( st.hover || st.active )
        set_mouse_cursor( APP_CURSOR_TEXT );

    u32 len = 0;
    while ( len < bufsz - 1u && buf[ len ] ) ++len;

    /* Clamp cursor and anchor to the current length -- a programmatic buffer change between
       frames may have shortened the string under the old positions. */
    if ( es->cursor > len ) es->cursor = (u16)len;
    if ( es->anchor > len ) es->anchor = (u16)len;

    u32  sel_lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
    u32  sel_hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
    bool has_sel = ( sel_lo != sel_hi );

    if ( focused )
    {
        /* On the first frame this field is focused, initialise the undo ring for it. */
        if ( s_undo.for_id != id )
            undo_init( &s_undo, id, buf, es->cursor, es->anchor );

        bool shift = io_shift();
        bool ctrl  = io_ctrl();
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
            es->cursor = es->anchor = (u16)sel_lo;
            has_sel = false; sel_lo = sel_hi = es->cursor;
            undo_push( &s_undo, buf, es->cursor, es->anchor );
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
                es->cursor = es->anchor = (u16)sel_lo;
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
            undo_push( &s_undo, buf, es->cursor, es->anchor );
            res.changed = true;
            blink_reset = true;
        }

        /* Undo / redo.  Ctrl+Z undoes; Ctrl+Y or Ctrl+Shift+Z redoes.  Both repeat. */
        if ( ctrl && !shift && s_io.keys_pressed_repeat[ APP_KEY_Z ] )
        {
            if ( s_undo.cur > 0 )
            {
                s_undo.cur--;
                res.changed     = undo_apply( &s_undo, s_undo.cur, buf, bufsz, es );
                s_undo.last_was_char = false;
                len             = 0;
                while ( len < bufsz - 1u && buf[ len ] ) ++len;
                has_sel = false; sel_lo = sel_hi = es->cursor;
                blink_reset = true;
            }
        }

        if ( ctrl && ( s_io.keys_pressed_repeat[ APP_KEY_Y ] ||
                       ( shift && s_io.keys_pressed_repeat[ APP_KEY_Z ] ) ) )
        {
            if ( s_undo.cur + 1 < s_undo.top )
            {
                s_undo.cur++;
                res.changed     = undo_apply( &s_undo, s_undo.cur, buf, bufsz, es );
                s_undo.last_was_char = false;
                len             = 0;
                while ( len < bufsz - 1u && buf[ len ] ) ++len;
                has_sel = false; sel_lo = sel_hi = es->cursor;
                blink_reset = true;
            }
        }

        /* Navigation: Left / Right collapse or extend the selection; Home / End jump.
           Ctrl+Left / Ctrl+Right jump by word.  Navigation and deletion read
           keys_pressed_repeat so a held key repeats at the OS rate. */

        if ( s_io.keys_pressed_repeat[ APP_KEY_LEFT ] )
        {
            if ( ctrl )
            {
                /* Word-jump left: skip whitespace run, then skip preceding word. */
                u32 pos = es->cursor;
                while ( pos > 0 && char_class( (u8)buf[ pos - 1u ] ) == 0 ) --pos;
                if ( pos > 0 )
                {
                    int cls = char_class( (u8)buf[ pos - 1u ] );
                    while ( pos > 0 && char_class( (u8)buf[ pos - 1u ] ) == cls ) --pos;
                }
                es->cursor = (u16)pos;
                if ( !shift ) es->anchor = (u16)pos;
            }
            else
            {
                if ( !shift && has_sel ) { es->cursor = es->anchor = (u16)sel_lo; }
                else if ( es->cursor > 0 ) { --es->cursor; if ( !shift ) es->anchor = es->cursor; }
            }
            blink_reset = true;
        }

        if ( s_io.keys_pressed_repeat[ APP_KEY_RIGHT ] )
        {
            if ( ctrl )
            {
                /* Word-jump right: skip current word, then skip trailing whitespace. */
                u32 pos = es->cursor;
                if ( pos < len )
                {
                    int cls = char_class( (u8)buf[ pos ] );
                    while ( pos < len && char_class( (u8)buf[ pos ] ) == cls ) ++pos;
                }
                while ( pos < len && char_class( (u8)buf[ pos ] ) == 0 ) ++pos;
                es->cursor = (u16)pos;
                if ( !shift ) es->anchor = (u16)pos;
            }
            else
            {
                if ( !shift && has_sel ) { es->cursor = es->anchor = (u16)sel_hi; }
                else if ( es->cursor < len ) { ++es->cursor; if ( !shift ) es->anchor = es->cursor; }
            }
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_HOME ] )
        {
            es->cursor = 0; if ( !shift ) es->anchor = 0;
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_END ] )
        {
            es->cursor = (u16)len; if ( !shift ) es->anchor = (u16)len;
            blink_reset = true;
        }

        /* Ctrl+A: select the entire buffer. */
        if ( ctrl && s_io.keys_pressed[ APP_KEY_A ] )
        {
            es->anchor = 0; es->cursor = (u16)len;
            blink_reset = true;
        }

        /* Backspace: delete the selection, or the character before the caret. */
        if ( s_io.keys_pressed_repeat[ APP_KEY_BACKSPACE ] )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = (u16)sel_lo;
                undo_push( &s_undo, buf, es->cursor, es->anchor );
                res.changed = true;
            }
            else if ( es->cursor > 0 )
            {
                --es->cursor;
                memmove( buf + es->cursor, buf + es->cursor + 1u, len - es->cursor );
                --len; buf[ len ] = '\0';
                es->anchor = es->cursor;
                undo_push( &s_undo, buf, es->cursor, es->anchor );
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
                es->cursor = es->anchor = (u16)sel_lo;
                undo_push( &s_undo, buf, es->cursor, es->anchor );
                res.changed = true;
            }
            else if ( es->cursor < len )
            {
                memmove( buf + es->cursor, buf + es->cursor + 1u, len - es->cursor );
                --len; buf[ len ] = '\0';
                undo_push( &s_undo, buf, es->cursor, es->anchor );
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
                /* Selection replacement ends the current char group (ring[cur] already holds
                   the pre-replacement state) but does not push a redundant duplicate. */
                s_undo.last_was_char = false;
                memmove( buf + sel_lo + 1u, buf + sel_hi, len - sel_hi + 1u );
                buf[ sel_lo ] = *ch;
                len          -= ( sel_hi - sel_lo ) - 1u;
                es->cursor    = es->anchor = (u16)( sel_lo + 1u );
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

        /* After character input, push or update the undo snapshot for char grouping.
           Push if this is the first char in a burst; update in place for subsequent chars. */
        if ( res.changed && !ctrl && s_io.text[ 0 ] )
        {
            if ( !s_undo.last_was_char )
            {
                undo_push( &s_undo, buf, es->cursor, es->anchor );
                s_undo.last_was_char = true;
            }
            else
            {
                undo_update( &s_undo, buf, es->cursor, es->anchor );
            }
        }

        /* Enter submits; Escape reverts to content at focus-gain, then drops focus. */
        if ( s_io.keys_pressed[ APP_KEY_ENTER ] )
        {
            s_undo.for_id    = IMGUI_ID_NONE;
            s_interaction.focused_id = IMGUI_ID_NONE;
            res.enter = true;
        }
        if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
        {
            /* Restore the buffer to its state at focus-gain, signal change if it differs. */
            u32 rv_len = 0;
            while ( rv_len < bufsz - 1u && s_undo.revert[ rv_len ] ) ++rv_len;
            if ( rv_len != len || memcmp( buf, s_undo.revert, rv_len ) != 0 )
            {
                memcpy( buf, s_undo.revert, rv_len + 1u );
                es->cursor = es->anchor = 0;
                res.changed = true;
                len = rv_len;
            }
            s_undo.for_id    = IMGUI_ID_NONE;
            s_interaction.focused_id = IMGUI_ID_NONE;
        }

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
                /* Double-click: select the word under the cursor.
                   text_offset_at places `off` AFTER a glyph when the click lands on its right
                   half, so clicking the last char of a word yields off = word_end (whitespace or
                   end-of-string).  Step back one to land inside the word in that case. */
                u32 wb_off = off;
                if ( wb_off > 0 &&
                     ( wb_off >= len || char_class( (u8)buf[ wb_off ] ) == 0 ) &&
                     char_class( (u8)buf[ wb_off - 1u ] ) != 0 )
                    wb_off--;
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
                    u32 drag_off = off;
                    if ( drag_off > 0 &&
                         ( drag_off >= len || char_class( (u8)buf[ drag_off ] ) == 0 ) &&
                         char_class( (u8)buf[ drag_off - 1u ] ) != 0 )
                        drag_off--;
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
        if ( cx - (f32)es->scroll_x < 0.0f )  es->scroll_x = (u16)cx;
        if ( cx - (f32)es->scroll_x > vis_w ) es->scroll_x = (u16)( cx - vis_w );
    }

    /* Clip text, selection, and caret to the box interior so scrolled content does not bleed past
       the border -- but ONLY when it actually would: the field is scrolled, or the text is wider
       than the visible interior.  A short, unscrolled value fits inside the box on its own, so it
       needs no scissor and stays merged into the surrounding window batch instead of forcing a
       draw-call split (the self-fit-over-clips rule -- clip only on real overflow). */
    f32  edit_vis_w  = box.w - 2.0f * WIDGET_PAD;
    bool need_clip   = es->scroll_x != 0 || text_x_at( buf, len ) > edit_vis_w;
    if ( need_clip )
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

    if ( need_clip )
        draw_pop_clip_rect();

    /* Fire the change callback after all rendering so the caller sees the final state. */
    if ( res.changed && on_change )
    {
        u32 final_len = 0;
        while ( final_len < bufsz - 1u && buf[ final_len ] ) ++final_len;
        on_change( buf, final_len, bufsz, cb_user );
    }

    return res;
}

// clang-format on
/*============================================================================================*/
