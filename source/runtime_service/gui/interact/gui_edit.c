/*==============================================================================================

    runtime_service/gui/interact/gui_edit.c -- single-line text edit engine.

    The complete behavior of a text field, minus the paint: buffer insert / delete, cursor +
    selection in BYTE space, word motion, the undo / redo ring, clipboard cut / copy / paste, the
    key -> command mapping, glyph MEASUREMENT (caret pixel-x, click pixel -> byte offset), the
    mouse selection drag that rides it, horizontal scroll to keep the caret in view, and the caret
    blink clock.  A keyboard-and-mouse interaction mechanism, sibling to move / resize / drag: it
    consumes (id, content rect, item state, buf, io) plus caller-owned edit state and produces
    DECISIONS -- a new buffer, cursor / anchor, scroll bias, blink phase -- never a pixel.

    Measurement is math over font metrics (the text/ leaf), not drawing, so it lives here: an
    interact mechanism may size and hit-test text exactly as it sizes and hit-tests any rect; it
    just never emits a glyph.  The one convenient entry is edit_field(): drive it once per frame
    over a content rect and it runs the whole non-paint frame.  The wrapping widget
    (chrome/widgets/gui_text_edit.c) supplies the box, colors, and frame chrome, then paints the
    resolved state (text / selection / caret) that this engine leaves on the edit-state slot.

    The measurement helpers (text_x_at / text_offset_at) and the pure byte-offset helpers
    (char_class / word_bounds / word_click_off / edit_strlen / edit_sel) are shared through the
    seam with the multiline wrapper (chrome/widgets/gui_text_edit_multi.c).

    The persisted per-id edit state (gui_edit_state_t) lives in the keyed state pool; the widget
    allocates it and hands this engine a pointer.  gui_clipboard_set / item_focus_release are the
    interact server's (core/gui_io.c, core/gui_item.c); cursor_set is core's (core/gui_ctx.c).

==============================================================================================*/
// clang-format off

/* gui_edit_state_t (the persisted per-id edit state) and input_field_result_t (the edit_field
   result) are defined in the unit seam, interact/gui_interact.h, so this engine, the single-line
   widget, and the multiline wrapper all share one definition. */

/* Character class for double-click word selection: a click extends over the maximal run of
   one class.  0 = whitespace, 1 = word (alphanumeric or underscore), 2 = punctuation/other --
   the classic "select word, or select a run of symbols" split.  Newlines count as whitespace
   for the multiline engine (gui_text_edit_multi.c); a single-line buffer never contains them. */
int
char_class( u8 c )
{
    if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' )        return 0;
    if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
         ( c >= '0' && c <= '9' ) || c == '_' )                   return 1;
    return 2;
}

/* Word bounds [*lo,*hi) around byte `off`: the run of same-class characters containing it.
   Used by double-click to snap the selection to a whole word.  A click past the end (off ==
   len) collapses to an empty range at len so a double-click in empty space selects nothing. */
void
word_bounds( const char* buf, u32 len, u32 off, u32* lo, u32* hi )
{
    if ( off >= len ) { *lo = *hi = len; return; }
    int cls = char_class( (u8)buf[ off ] );
    u32 a = off, b = off;
    while ( a > 0   && char_class( (u8)buf[ a - 1u ] ) == cls ) --a;
    while ( b < len && char_class( (u8)buf[ b ]      ) == cls ) ++b;
    *lo = a; *hi = b;
}

/* Right-edge correction for a word-select click: text_offset_at places `off` AFTER a glyph when
   the click lands on its right half, so clicking the last char of a word yields off == word_end
   (whitespace or end-of-string).  Step back one to land inside the word in that case.  Shared by
   the double-click and the word-select drag, which must correct identically. */
u32
word_click_off( const char* buf, u32 len, u32 off )
{
    bool past_word = ( off >= len ) || ( char_class( (u8)buf[ off ] ) == 0 );
    if ( off > 0 && past_word && char_class( (u8)buf[ off - 1u ] ) != 0 )
        --off;
    return off;
}

/*==============================================================================================
    Glyph measurement -- math over the active font's advances (the text/ leaf), the interact-side
    counterpart of a hit-test.  Shared with both widget wrappers through the seam so the caret,
    the click mapping, and the paint all agree on where each byte sits.  No drawing.
==============================================================================================*/

/* Pixel x-offset of the insertion point at byte index `off` in `buf`, measured from the left
   edge of the first glyph (scroll is not applied here; the caller adjusts).  Stops safely at a
   NUL so off > len is handled without bounds checks. */
f32
text_x_at( const char* buf, u32 off )
{
    f32 x = 0.0f;
    for ( u32 i = 0; i < off && buf[ i ]; ++i )
        x += font_char_advance( (u8)buf[ i ] );
    return x;
}

/* Byte offset in buf[0..len) nearest to pixel position `px` measured from the text origin.
   Snaps to the midpoint of each glyph so a click in the left half of a glyph lands before it
   and in the right half lands after it, matching standard click-to-caret behaviour. */
u32
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

/*==============================================================================================
    Undo / redo ring buffer -- single global, keyed to the focused widget.

    Only one text field can hold focus at a time, so a single ring buffer keyed by focused_id
    is sufficient.  The ring stores up to UNDO_SLOTS snapshots of the full buffer content plus
    cursor/anchor; each snapshot fits in UNDO_TEXT_MAX bytes.

    Snapshots are pushed AFTER each committed edit, storing the caret state after that edit.
    Grouping: consecutive single-character insertions merge into one undo step (the snapshot for
    the current group is updated in place) so a long typing burst undoes as a whole word.  Any
    other edit (paste, delete, cut) breaks the group.  Escape-to-revert uses a separate `revert`
    copy saved at focus-gain, which survives ring wrapping and is always the unmodified content.
==============================================================================================*/

#define UNDO_SLOTS    16
#define UNDO_TEXT_MAX 256

typedef struct
{
    char text[ UNDO_TEXT_MAX ];
    u16  cursor;
    u16  anchor;
} gui_undo_snap_t;

typedef struct
{
    gui_id_t            for_id;                   // which widget owns this history
    char                revert[ UNDO_TEXT_MAX ];  // buffer content at focus-gain
    gui_undo_snap_t     ring[ UNDO_SLOTS ];       // circular snapshot ring
    i32                 base;                     // ring index of logical slot 0
    i32                 cur;                      // logical index of the current (live) snapshot
    i32                 top;                      // one past the highest committed logical index
    bool                last_was_char;            // true when the last push was a char insert

} gui_undo_buf_t;

static gui_undo_buf_t s_undo;

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
undo_push( gui_undo_buf_t* u, const char* text, u16 cursor, u16 anchor )
{
    u->cur++;
    if ( u->cur == UNDO_SLOTS )
    {
        u->base = ( u->base + 1 ) % UNDO_SLOTS;
        u->cur  = UNDO_SLOTS - 1;
    }
    gui_undo_snap_t* s = &u->ring[ ( u->base + u->cur ) % UNDO_SLOTS ];
    undo_text_copy( s->text, text );
    s->cursor        = cursor;
    s->anchor        = anchor;
    u->top           = u->cur + 1;
    u->last_was_char = false;
}

/* Overwrite the current snapshot in place (char-insert grouping: extend the current word
   without creating a new undo step). */
static void
undo_update( gui_undo_buf_t* u, const char* text, u16 cursor, u16 anchor )
{
    gui_undo_snap_t* s = &u->ring[ ( u->base + u->cur ) % UNDO_SLOTS ];
    undo_text_copy( s->text, text );
    s->cursor = cursor;
    s->anchor = anchor;
}

/* Initialize the undo ring for a newly focused widget.  Saves the revert copy and pushes
   the initial state as the floor of the undo stack. */
static void
undo_init( gui_undo_buf_t* u, gui_id_t id, const char* buf, u16 cursor, u16 anchor )
{
    u->for_id        = id;
    u->base          = 0;
    u->cur           = 0;
    u->top           = 1;
    u->last_was_char = false;
    undo_text_copy( u->revert, buf );
    gui_undo_snap_t* s = &u->ring[ 0 ];
    undo_text_copy( s->text, buf );
    s->cursor = cursor;
    s->anchor = anchor;
}

/* Apply a ring snapshot to buf/es; safe if the snapshot text is longer than bufsz (truncates).
   Returns true so callers can chain: res.changed = undo_apply(...). */
static bool
undo_apply( gui_undo_buf_t* u, i32 logical_idx, char* buf, u32 bufsz,
            gui_edit_state_t* es )
{
    gui_undo_snap_t* s   = &u->ring[ ( u->base + logical_idx ) % UNDO_SLOTS ];
    u32                n   = 0;
    while ( n < bufsz - 1u && s->text[ n ] ) ++n;
    memcpy( buf, s->text, n );
    buf[ n ]   = '\0';
    es->cursor = ( s->cursor <= n ) ? s->cursor : (u16)n;
    es->anchor = ( s->anchor <= n ) ? s->anchor : (u16)n;
    return true;
}

/* Programmatic caret request (public: gui()->set_edit_cursor_end).  Latched until the next
   FOCUSED field passes through edit_keys, which seats the caret at the end of the buffer with
   no selection.  Pairs with a programmatic buffer replacement (history recall, tab completion):
   without it the caret stays wherever it sat in the old text. */

static bool s_cursor_end_request = false;

void
gui_set_edit_cursor_end( void )
{
    s_cursor_end_request = true;
}

/* Key passthrough hook (public: gui()->set_edit_key_hook).  The next FOCUSED field to run
   consumes the registration and calls the hook for every key down this frame before its own
   key handling; a key the hook consumes is cleared from the frame io.  One-shot by design --
   re-register just before emitting the field each frame -- so a hook can never dangle on a
   field it was not meant for. */

static gui_edit_key_fn s_key_hook      = NULL;
static void*           s_key_hook_user = NULL;

void
gui_set_edit_key_hook( gui_edit_key_fn fn, void* user )
{
    s_key_hook      = fn;
    s_key_hook_user = user;
}

/* Length of a NUL-terminated string bounded by its buffer capacity (never counts the terminator,
   never runs past bufsz-1).  The field recomputes this wherever an edit may have resized buf. */
u32
edit_strlen( const char* buf, u32 bufsz )
{
    u32 n = 0;
    while ( n < bufsz - 1u && buf[ n ] ) ++n;
    return n;
}

/* Selection bounds from the caret/anchor pair: [lo,hi) is the highlighted range, *has is false
   for a bare insertion point (cursor == anchor). */
void
edit_sel( const gui_edit_state_t* es, u32* lo, u32* hi, bool* has )
{
    *lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
    *hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
    *has = ( *lo != *hi );
}

/* Key passthrough hook: the registered one-shot hook gets first crack at every key down this
   frame (history recall, tab completion, scrollback jumps).  A key the hook consumes is erased
   from the frame io so the field's own handling never sees it.  The hook may replace the buffer
   (and queue set_edit_cursor_end), so length and caret are recomputed afterward; wants_redraw
   covers hook-side state the caller emitted before this field ran (scrollback rows), or the
   clean-frame skip would stall the update. */

static void
edit_run_key_hook( char* buf, u32 bufsz, gui_edit_state_t* es, u32* len_io )
{
    if ( !s_key_hook ) return;

    gui_edit_key_fn hook = s_key_hook;
    void*           user = s_key_hook_user;
    bool            used = false;
    s_key_hook      = NULL;
    s_key_hook_user = NULL;

    for ( u32 k = 0; k < GUI_KEY_COUNT; ++k )
    {
        if ( !s_io.keys_pressed_repeat[ k ] ) continue;
        if ( hook( k, io_ctrl(), io_shift(), !s_io.keys_pressed[ k ], user ) )
        {
            key_claim( (app_key_t)k );   /* the hook consumed it -- the field must not also see it */
            used = true;
        }
    }

    if ( used )
    {
        u32 len = edit_strlen( buf, bufsz );
        if ( es->cursor > len ) es->cursor = (u16)len;
        if ( es->anchor > len ) es->anchor = (u16)len;
        *len_io = len;
        redraw_request();
    }
}

/* Keyboard command handling for a focused field: clipboard copy/cut/paste, undo/redo, caret
   navigation (Left/Right/Home/End, word jumps, Ctrl+A), backspace/delete, character insertion,
   and the Enter/Escape submit/revert.  Threads len (kept in step with buf), the result flags,
   and the blink-reset request back to the caller. */

static void
edit_apply_keys( char* buf, u32 bufsz, gui_edit_state_t* es, bool ctrl, bool shift,
                 u32* len_io, input_field_result_t* res_io, bool* blink_io )
{
    u32                  len         = *len_io;
    input_field_result_t res         = *res_io;
    bool                 blink_reset = *blink_io;

    u32  sel_lo, sel_hi;
    bool has_sel;
    edit_sel( es, &sel_lo, &sel_hi, &has_sel );

    /* Clipboard.  Copy / cut are key-driven (only this field knows the selection) and push
       to the OS clipboard via gui_clipboard_set.  Paste is event-driven: the platform
       already read the OS clipboard on the paste gesture and delivered it in s_io.paste, so
       there is no Ctrl+V key check here -- a non-empty s_io.paste IS the paste.  Resolved
       first so it acts on the selection as the user sees it, before navigation moves it. */

    if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_C ] )
    {
        gui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
        blink_reset = true;
    }

    if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_X ] )
    {
        gui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
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
            len             = edit_strlen( buf, bufsz );
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
            len             = edit_strlen( buf, bufsz );
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
       Ctrl is held so shortcut combos (Ctrl+C/V/X/A) never leak a stray glyph. */
    for ( const char* ch = ctrl ? "" : s_io.text; *ch; ++ch )
    {
        /* Single-line field: reject control characters (tab, CR, ...) exactly like the
           paste path above -- a consumed Tab must not leak a '\t' glyph into the buffer. */
        if ( (u8)*ch < 0x20u || (u8)*ch == 0x7Fu ) continue;

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
        s_undo.for_id    = GUI_ID_NONE;
        item_focus_release();
        res.enter = true;
    }
    if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
    {
        /* Restore the buffer to its state at focus-gain, signal change if it differs. */
        u32 rv_len = edit_strlen( s_undo.revert, bufsz );
        if ( rv_len != len || memcmp( buf, s_undo.revert, rv_len ) != 0 )
        {
            memcpy( buf, s_undo.revert, rv_len + 1u );
            es->cursor = es->anchor = 0;
            res.changed = true;
            len = rv_len;
        }
        s_undo.for_id    = GUI_ID_NONE;
        item_focus_release();
    }

    *len_io   = len;
    *res_io   = res;
    *blink_io = blink_reset;
}

/*==============================================================================================
    Mouse selection -- click-to-caret, Shift-extend, double-click word select, and the click-drag
    that extends the selection (plain, or by whole words after a double-click).  Runs off st.active
    so the drag survives the cursor leaving the box, like the scrollbar knob.  Measurement
    (text_offset_at) turns the cursor pixel into a byte offset in content space -- content.x is the
    text origin, so the scroll bias is added back in -- then the word helpers snap to boundaries.
==============================================================================================*/

static void
edit_apply_mouse( gui_rect_t content, gui_item_state_t st, char* buf, u32 len,
                  gui_edit_state_t* es, bool shift, bool* blink_io )
{
    bool blink_reset = *blink_io;

    /* st.pressed is the grab frame (also the focus-gaining click, since item_state set
       focused_id = id by now); st.active stays true for the whole capture, so the drag below
       keeps extending the selection even after the cursor leaves the box.  text_offset_at clamps
       a cursor past either edge to 0 / len, so a drag past the ends selects to start / end. */

    f32 px  = s_io.mouse_x - content.x + es->scroll_x;
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

/*==============================================================================================
    Scroll-into-view -- bias the content horizontally so the caret stays inside the visible width.
    Runs every frame (focused or not) so a programmatic caret move from outside is also honoured;
    content.w is the visible text width (the box already inset by the widget).
==============================================================================================*/

static void
edit_scroll( gui_rect_t content, const char* buf, gui_edit_state_t* es )
{
    f32 cx    = text_x_at( buf, es->cursor );
    f32 vis_w = content.w;
    if ( vis_w < 0.0f ) vis_w = 0.0f;
    if ( cx - (f32)es->scroll_x < 0.0f )  es->scroll_x = (u16)cx;
    if ( cx - (f32)es->scroll_x > vis_w ) es->scroll_x = (u16)( cx - vis_w );
}

/*==============================================================================================
    edit_field -- the one convenient entry: a full non-paint frame of a single-line text field.

    Fetches the field's persisted edit state, requests the I-beam cursor over the content, and --
    while focused -- runs the key hook, honours a queued set_edit_cursor_end, initialises the undo
    ring on first focus, publishes the live-selection fact for window-level text selection, applies
    every key command, then the mouse selection drag, then advances the caret-blink clock.  Every
    frame (focused or not) it scrolls the caret into view so a programmatic move is honoured, and
    reports any change to the item record.  Returns { changed, enter } and leaves cursor / anchor /
    scroll_x / blink_t on the state slot for the wrapping widget to paint; the widget supplies the
    content rect, the colors, and the box chrome, and does nothing else.
==============================================================================================*/
input_field_result_t
edit_field( gui_id_t id, gui_rect_t content, gui_item_state_t st, char* buf, u32 bufsz )
{
    gui_edit_state_t*    es  = GUI_STATE( gui_edit_state_t, id );
    input_field_result_t res = { false, false };
    u32                  len = edit_strlen( buf, bufsz );

    /* I-beam over a text field -- held through a selection drag (st.active), so it does not flip
       back to the arrow while the cursor sweeps outside the box mid-drag. */
    if ( st.hover || st.active )
        cursor_set( APP_CURSOR_TEXT );

    /* Clamp caret / anchor to the current length -- a programmatic buffer change between frames
       may have shortened the string under the old positions. */
    if ( es->cursor > len ) es->cursor = (u16)len;
    if ( es->anchor > len ) es->anchor = (u16)len;

    if ( st.focused )
    {
        bool blink_reset = false;

        /* The registered hook gets first crack at this frame's keys (may replace buf, moving len). */
        edit_run_key_hook( buf, bufsz, es, &len );

        /* A queued set_edit_cursor_end request lands on the focused field: caret to the end of the
           (possibly replaced) buffer, selection collapsed, blink reset so the caret shows. */
        if ( s_cursor_end_request )
        {
            s_cursor_end_request = false;
            es->cursor  = es->anchor = (u16)len;
            es->blink_t = 0.0f;
        }

        /* On the first frame this field is focused, initialise the undo ring for it. */
        if ( s_undo.for_id != id )
            undo_init( &s_undo, id, buf, es->cursor, es->anchor );

        /* Publish whether this focused field holds a live selection, so a window-level text
           selection (GUI_WIN_TEXT_SELECT) yields Ctrl+C to the field's own copy and only takes it
           when the field has none.  interact/ is a sanctioned writer of the s_interaction record. */
        s_interaction.focus_has_selection = ( es->cursor != es->anchor );

        edit_apply_keys( buf, bufsz, es, io_ctrl(), io_shift(), &len, &res, &blink_reset );
        len = edit_strlen( buf, bufsz );   /* keys may have resized buf under us */

        edit_apply_mouse( content, st, buf, len, es, io_shift(), &blink_reset );

        /* Caret blink clock: any activity this frame un-hides the caret; otherwise it advances. */
        if ( blink_reset ) es->blink_t  = 0.0f;
        else               es->blink_t += s_io.dt;
    }

    /* Scroll the caret into view every frame so a programmatic move from outside is honoured. */
    edit_scroll( content, buf, es );

    /* Report the edit to the item record (is_item_deactivated_after_edit, core/gui_query.c). */
    if ( res.changed )
        item_mark_edited();

    return res;
}

// clang-format on
/*============================================================================================*/
