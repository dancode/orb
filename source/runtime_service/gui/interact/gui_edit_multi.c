/*==============================================================================================

    runtime_service/gui/interact/gui_edit_multi.c -- multi-line text edit engine.

    The field-internal half of input_text_multiline, the two-dimensional twin of gui_edit.c: the
    '\n'-separated buffer, the caret + selection in BYTE space moving in two dimensions (a sticky
    preferred column for Up / Down), word motion, a private 2 KB undo / redo ring, clipboard with
    newline-preserving paste, the key -> command map, glyph + line-geometry MEASUREMENT, the mouse
    selection drag, the horizontal pan that chases the caret, and the caret-blink clock.  Like the
    single-line engine it consumes (id, content rect, item state, buf, io) and produces DECISIONS,
    never a pixel; measurement over the font/ resource is math, not drawing, so it lives here.

    What stays with the wrapping widget (chrome/widgets/gui_text_edit_multi.c) is exactly what an
    interact mechanism may not reach: the VERTICAL scroll, which belongs to the enclosing child
    REGION (flow, above this layer) -- the widget writes the region's scroll_y to chase the caret's
    row -- and all painting.  The one entry is medit_edit(): drive it once per frame over the inner
    content rect and it runs the whole field-internal frame, leaving caret / anchor / scroll_x /
    blink on the keyed state slot for the widget to chase and paint.

    Enter inserts a newline here (it never submits or drops focus -- Escape reverts and leaves,
    like the single-line field); char_class treats '\n' as whitespace, so the shared word ops
    (char_class / word_bounds / word_click_off) cross lines naturally.  The undo ring is a private
    2 KB twin of the single-line ring (that one is 256 bytes -- too small for an editor buffer)
    with one extra rule: a buffer too large for a snapshot slot marks the history dead until the
    next focus gain, so undo never restores a truncated buffer.

==============================================================================================*/
// clang-format off

/* gui_medit_state_t (the persisted per-id editor state) and medit_result_t (the medit_edit
   result) are defined in the unit seam, interact/gui_interact.h, so this engine and the widget
   share one definition. */

/* Selection bounds from the caret/anchor pair (u32 twin of edit_sel). */
void
medit_sel( const gui_medit_state_t* es, u32* lo, u32* hi, bool* has )
{
    *lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
    *hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
    *has = ( *lo != *hi );
}

/*==============================================================================================
    Line geometry -- byte-offset <-> (row, pixel-x) mapping over the '\n'-separated buffer.

    All linear scans: an editor-sized buffer (a few KB) rescans in negligible time, and the
    scans only run on frames with caret or paint activity for the focused / visible field.  The
    row/end/start/caret_rowx readers are shared with the widget's paint + vertical chase.
==============================================================================================*/

/* Number of display lines: '\n' count + 1 (an empty buffer is one empty line). */
u32
medit_line_count( const char* buf, u32 len )
{
    u32 n = 1;
    for ( u32 i = 0; i < len; ++i )
        if ( buf[ i ] == '\n' ) ++n;
    return n;
}

/* Start of the line containing byte `off` (one past the previous '\n', or 0). */
static u32
medit_line_start( const char* buf, u32 off )
{
    while ( off > 0 && buf[ off - 1u ] != '\n' ) --off;
    return off;
}

/* End of the line containing byte `off`: the offset of its '\n', or len on the last line. */
u32
medit_line_end( const char* buf, u32 len, u32 off )
{
    while ( off < len && buf[ off ] != '\n' ) ++off;
    return off;
}

/* Start offset of display line `row`, clamped to the last line when row overshoots. */
u32
medit_row_start( const char* buf, u32 len, u32 row )
{
    u32 off = 0;
    while ( row > 0 )
    {
        u32 e = medit_line_end( buf, len, off );
        if ( e >= len ) break;                    /* past the last line: clamp to its start */
        off = e + 1u;
        --row;
    }
    return off;
}

/* Row index and pixel x of the caret at byte `off`. */
void
medit_caret_rowx( const char* buf, u32 off, u32* row, f32* x )
{
    u32 r = 0;
    for ( u32 i = 0; i < off; ++i )
        if ( buf[ i ] == '\n' ) ++r;
    *row = r;
    u32 ls = medit_line_start( buf, off );
    *x   = text_x_at( buf + ls, off - ls );
}

/* Byte offset nearest to (row, px), with row clamped into the line range and px clamped left.
   Reuses text_offset_at within the resolved line (glyph-midpoint snapping). */
static u32
medit_offset_at( const char* buf, u32 len, i32 row, f32 px )
{
    if ( row < 0 ) row = 0;
    u32 nlines = medit_line_count( buf, len );
    if ( (u32)row >= nlines ) row = (i32)nlines - 1;
    u32 ls = medit_row_start( buf, len, (u32)row );
    u32 le = medit_line_end( buf, len, ls );
    return ls + text_offset_at( buf + ls, le - ls, px > 0.0f ? px : 0.0f );
}

/*==============================================================================================
    Undo / redo ring -- private multiline twin of the single-line ring (interact/gui_edit.c).

    Same shape (snapshots after committed edits, char-burst grouping, Escape-revert copy at
    focus gain) with a bigger slot and one extra rule: a buffer that no longer fits a slot
    marks the whole history DEAD until the next focus gain.  Undo must never restore a
    truncated snapshot over a longer live buffer, so an oversize edit simply switches undo
    off rather than corrupting the text.  revert_ok gates Escape the same way.
==============================================================================================*/

#define MEDIT_UNDO_SLOTS    8
#define MEDIT_UNDO_TEXT_MAX 2048

typedef struct
{
    char text[ MEDIT_UNDO_TEXT_MAX ];
    u32  cursor;
    u32  anchor;
} medit_undo_snap_t;

typedef struct
{
    gui_id_t          for_id;                        // which widget owns this history
    char              revert[ MEDIT_UNDO_TEXT_MAX ]; // buffer content at focus-gain
    medit_undo_snap_t ring[ MEDIT_UNDO_SLOTS ];      // circular snapshot ring
    i32               base;                          // ring index of logical slot 0
    i32               cur;                           // logical index of the current snapshot
    i32               top;                           // one past the highest committed index
    bool              last_was_char;                 // true when the last push was a char insert
    bool              revert_ok;                     // the revert copy captured the full buffer
    bool              dead;                          // an edit outgrew a slot; history disabled

} medit_undo_buf_t;

static medit_undo_buf_t s_medit_undo;

/* NUL-bounded copy into a snapshot slot; returns false (without writing) when src does not fit. */
static bool
medit_undo_copy( char* dst, const char* src )
{
    u32 n = 0;
    while ( n < MEDIT_UNDO_TEXT_MAX && src[ n ] ) ++n;
    if ( n >= MEDIT_UNDO_TEXT_MAX ) return false;
    memcpy( dst, src, n );
    dst[ n ] = '\0';
    return true;
}

/* Advance the ring and write a snapshot; an oversize buffer kills the history instead. */
static void
medit_undo_push( medit_undo_buf_t* u, const char* text, u32 cursor, u32 anchor )
{
    if ( u->dead ) return;
    u->cur++;
    if ( u->cur == MEDIT_UNDO_SLOTS )
    {
        u->base = ( u->base + 1 ) % MEDIT_UNDO_SLOTS;
        u->cur  = MEDIT_UNDO_SLOTS - 1;
    }
    medit_undo_snap_t* s = &u->ring[ ( u->base + u->cur ) % MEDIT_UNDO_SLOTS ];
    if ( !medit_undo_copy( s->text, text ) ) { u->dead = true; return; }
    s->cursor        = cursor;
    s->anchor        = anchor;
    u->top           = u->cur + 1;
    u->last_was_char = false;
}

/* Overwrite the current snapshot in place (char-burst grouping). */
static void
medit_undo_update( medit_undo_buf_t* u, const char* text, u32 cursor, u32 anchor )
{
    if ( u->dead ) return;
    medit_undo_snap_t* s = &u->ring[ ( u->base + u->cur ) % MEDIT_UNDO_SLOTS ];
    if ( !medit_undo_copy( s->text, text ) ) u->dead = true;
    else { s->cursor = cursor; s->anchor = anchor; }
}

/* Initialise the history for a newly focused widget: revert copy + floor snapshot. */
static void
medit_undo_init( medit_undo_buf_t* u, gui_id_t id, const char* buf, u32 cursor, u32 anchor )
{
    u->for_id        = id;
    u->base          = 0;
    u->cur           = 0;
    u->top           = 1;
    u->last_was_char = false;
    u->dead          = false;
    u->revert_ok     = medit_undo_copy( u->revert, buf );
    medit_undo_snap_t* s = &u->ring[ 0 ];
    if ( !medit_undo_copy( s->text, buf ) ) { u->dead = true; return; }
    s->cursor = cursor;
    s->anchor = anchor;
}

/* Apply a ring snapshot to buf/es (truncating only against the caller's bufsz). */
static bool
medit_undo_apply( medit_undo_buf_t* u, i32 logical_idx, char* buf, u32 bufsz,
                  gui_medit_state_t* es )
{
    medit_undo_snap_t* s = &u->ring[ ( u->base + logical_idx ) % MEDIT_UNDO_SLOTS ];
    u32                n = 0;
    while ( n < bufsz - 1u && s->text[ n ] ) ++n;
    memcpy( buf, s->text, n );
    buf[ n ]   = '\0';
    es->cursor = ( s->cursor <= n ) ? s->cursor : n;
    es->anchor = ( s->anchor <= n ) ? s->anchor : n;
    return true;
}

/*==============================================================================================
    Edit primitives -- the two buffer mutations every path shares.
==============================================================================================*/

/* Delete bytes [lo,hi) and collapse the caret to lo. */
static void
medit_erase( char* buf, u32* len, gui_medit_state_t* es, u32 lo, u32 hi )
{
    memmove( buf + lo, buf + hi, *len - hi + 1u );
    *len      -= ( hi - lo );
    es->cursor = es->anchor = lo;
}

/* Insert one byte at the caret; false (untouched) when the buffer is full. */
static bool
medit_insert( char* buf, u32 bufsz, u32* len, gui_medit_state_t* es, char ch )
{
    if ( *len + 1u >= bufsz ) return false;
    memmove( buf + es->cursor + 1u, buf + es->cursor, *len - es->cursor + 1u );
    buf[ es->cursor ] = ch;
    ++*len;
    ++es->cursor;
    es->anchor = es->cursor;
    return true;
}

/* Vertical caret move by `drow` display lines, aiming at the sticky preferred column.
   Up on the first line pins to offset 0 and Down on the last pins to len (the standard
   "nothing above / below" caret behavior).  Horizontal moves and edits invalidate pref_x;
   this is the only reader, and it latches the column on the first move of a vertical run. */
static void
medit_move_vert( const char* buf, u32 len, gui_medit_state_t* es, i32 drow, bool shift )
{
    u32 row; f32 x;
    medit_caret_rowx( buf, es->cursor, &row, &x );
    if ( !es->pref_valid ) { es->pref_x = x; es->pref_valid = 1; }

    i32 target = (i32)row + drow;
    if ( target < 0 )
    {
        es->cursor = 0;
    }
    else if ( (u32)target >= medit_line_count( buf, len ) )
    {
        es->cursor = len;
    }
    else
    {
        u32 ls = medit_row_start( buf, len, (u32)target );
        u32 le = medit_line_end( buf, len, ls );
        es->cursor = ls + text_offset_at( buf + ls, le - ls, es->pref_x );
    }
    if ( !shift ) es->anchor = es->cursor;
}

/*==============================================================================================
    Keyboard -- clipboard, undo / redo, 2D navigation, deletion, insertion, Escape-revert.

    Byte-for-byte the single-line contract where the semantics agree (Left / Right, word
    jumps, Ctrl+A, Backspace / Delete, char bursts); the multiline differences:
        - Enter inserts '\n' (repeats); it never submits or drops focus
        - Up / Down / PageUp / PageDown move the caret vertically (Shift extends)
        - Home / End are line-local; Ctrl+Home / Ctrl+End jump to the buffer ends
        - paste keeps newlines (CRLF / CR normalised to '\n', tabs expand to 4 spaces)
    char_class treats '\n' as whitespace, so the shared word ops cross lines naturally.
==============================================================================================*/

static void
medit_apply_keys( char* buf, u32 bufsz, gui_medit_state_t* es, bool ctrl, bool shift,
                  u32 vis_rows, u32* len_io, bool* changed_io, bool* blink_io )
{
    u32  len     = *len_io;
    bool changed = *changed_io;
    bool blink   = *blink_io;

    u32  sel_lo, sel_hi;
    bool has_sel;
    medit_sel( es, &sel_lo, &sel_hi, &has_sel );

    /* Clipboard.  Copy / cut are key-driven; paste is event-driven (s_io.paste IS the paste,
       exactly like the single-line field) -- resolved first, on the selection as the user
       sees it. */

    if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_C ] )
    {
        gui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
        blink = true;
    }

    if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_X ] )
    {
        gui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
        medit_erase( buf, &len, es, sel_lo, sel_hi );
        has_sel = false; sel_lo = sel_hi = es->cursor;
        medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
        changed = true;
        blink   = true;
        es->pref_valid = 0;
    }

    if ( s_io.paste[ 0 ] )
    {
        if ( has_sel )
        {
            medit_erase( buf, &len, es, sel_lo, sel_hi );
            has_sel = false; sel_lo = sel_hi = es->cursor;
        }
        /* Newlines survive a multiline paste: CRLF and lone CR normalise to '\n', tabs expand
           to four spaces (the atlas has no tab glyph), other control bytes drop. */
        for ( const char* c = s_io.paste; *c; ++c )
        {
            char ch = *c;
            if ( ch == '\r' )
            {
                if ( c[ 1 ] == '\n' ) continue;   /* CRLF: the LF carries the break */
                ch = '\n';
            }
            if ( ch == '\t' )
            {
                for ( int sp = 0; sp < 4; ++sp )
                    if ( !medit_insert( buf, bufsz, &len, es, ' ' ) ) break;
                continue;
            }
            if ( ( (u8)ch < 0x20u && ch != '\n' ) || (u8)ch == 0x7Fu ) continue;
            if ( !medit_insert( buf, bufsz, &len, es, ch ) ) break;
        }
        medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
        changed = true;
        blink   = true;
        es->pref_valid = 0;
    }

    /* Undo / redo.  Ctrl+Z undoes; Ctrl+Y or Ctrl+Shift+Z redoes.  Both repeat.  A dead
       history (oversize buffer) denies both rather than restoring a truncated snapshot. */
    if ( ctrl && !shift && s_io.keys_pressed_repeat[ APP_KEY_Z ] )
    {
        if ( !s_medit_undo.dead && s_medit_undo.cur > 0 )
        {
            s_medit_undo.cur--;
            changed = medit_undo_apply( &s_medit_undo, s_medit_undo.cur, buf, bufsz, es );
            s_medit_undo.last_was_char = false;
            len = edit_strlen( buf, bufsz );
            has_sel = false; sel_lo = sel_hi = es->cursor;
            blink = true;
            es->pref_valid = 0;
        }
    }

    if ( ctrl && ( s_io.keys_pressed_repeat[ APP_KEY_Y ] ||
                   ( shift && s_io.keys_pressed_repeat[ APP_KEY_Z ] ) ) )
    {
        if ( !s_medit_undo.dead && s_medit_undo.cur + 1 < s_medit_undo.top )
        {
            s_medit_undo.cur++;
            changed = medit_undo_apply( &s_medit_undo, s_medit_undo.cur, buf, bufsz, es );
            s_medit_undo.last_was_char = false;
            len = edit_strlen( buf, bufsz );
            has_sel = false; sel_lo = sel_hi = es->cursor;
            blink = true;
            es->pref_valid = 0;
        }
    }

    /* Horizontal navigation -- the single-line moves, byte-identical ('\n' is just another
       whitespace byte to step over, so Left at a line start lands on the previous line end). */

    if ( s_io.keys_pressed_repeat[ APP_KEY_LEFT ] )
    {
        if ( ctrl )
        {
            u32 pos = es->cursor;
            while ( pos > 0 && char_class( (u8)buf[ pos - 1u ] ) == 0 ) --pos;
            if ( pos > 0 )
            {
                int cls = char_class( (u8)buf[ pos - 1u ] );
                while ( pos > 0 && char_class( (u8)buf[ pos - 1u ] ) == cls ) --pos;
            }
            es->cursor = pos;
            if ( !shift ) es->anchor = pos;
        }
        else
        {
            if ( !shift && has_sel ) { es->cursor = es->anchor = sel_lo; }
            else if ( es->cursor > 0 ) { --es->cursor; if ( !shift ) es->anchor = es->cursor; }
        }
        blink = true;
        es->pref_valid = 0;
    }

    if ( s_io.keys_pressed_repeat[ APP_KEY_RIGHT ] )
    {
        if ( ctrl )
        {
            u32 pos = es->cursor;
            if ( pos < len )
            {
                int cls = char_class( (u8)buf[ pos ] );
                while ( pos < len && char_class( (u8)buf[ pos ] ) == cls ) ++pos;
            }
            while ( pos < len && char_class( (u8)buf[ pos ] ) == 0 ) ++pos;
            es->cursor = pos;
            if ( !shift ) es->anchor = pos;
        }
        else
        {
            if ( !shift && has_sel ) { es->cursor = es->anchor = sel_hi; }
            else if ( es->cursor < len ) { ++es->cursor; if ( !shift ) es->anchor = es->cursor; }
        }
        blink = true;
        es->pref_valid = 0;
    }

    /* Vertical navigation -- caret by line (Shift extends), page by the visible row count. */

    if ( s_io.keys_pressed_repeat[ APP_KEY_UP ] )
    {
        medit_move_vert( buf, len, es, -1, shift );
        blink = true;
    }

    if ( s_io.keys_pressed_repeat[ APP_KEY_DOWN ] )
    {
        medit_move_vert( buf, len, es, +1, shift );
        blink = true;
    }

    if ( s_io.keys_pressed_repeat[ APP_KEY_PAGE_UP ] )
    {
        medit_move_vert( buf, len, es, -(i32)vis_rows, shift );
        blink = true;
    }

    if ( s_io.keys_pressed_repeat[ APP_KEY_PAGE_DOWN ] )
    {
        medit_move_vert( buf, len, es, (i32)vis_rows, shift );
        blink = true;
    }

    /* Home / End are line-local; Ctrl jumps to the buffer ends. */

    if ( s_io.keys_pressed[ APP_KEY_HOME ] )
    {
        es->cursor = ctrl ? 0 : medit_line_start( buf, es->cursor );
        if ( !shift ) es->anchor = es->cursor;
        blink = true;
        es->pref_valid = 0;
    }

    if ( s_io.keys_pressed[ APP_KEY_END ] )
    {
        es->cursor = ctrl ? len : medit_line_end( buf, len, es->cursor );
        if ( !shift ) es->anchor = es->cursor;
        blink = true;
        es->pref_valid = 0;
    }

    /* Ctrl+A: select the entire buffer. */
    if ( ctrl && s_io.keys_pressed[ APP_KEY_A ] )
    {
        es->anchor = 0; es->cursor = len;
        blink = true;
    }

    /* Deletion and insertion act on the selection as navigation left it: refresh the bounds
       (the horizontal / vertical moves above may have collapsed or extended it this frame). */
    medit_sel( es, &sel_lo, &sel_hi, &has_sel );

    /* Backspace: delete the selection, or the byte before the caret ('\n' included, which
       joins the line onto the previous one). */
    if ( s_io.keys_pressed_repeat[ APP_KEY_BACKSPACE ] )
    {
        if ( has_sel )
        {
            medit_erase( buf, &len, es, sel_lo, sel_hi );
            medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
            changed = true;
        }
        else if ( es->cursor > 0 )
        {
            medit_erase( buf, &len, es, es->cursor - 1u, es->cursor );
            medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
            changed = true;
        }
        has_sel = false; sel_lo = sel_hi = es->cursor;
        blink = true;
        es->pref_valid = 0;
    }

    /* Delete: delete the selection, or the byte after the caret. */
    if ( s_io.keys_pressed_repeat[ APP_KEY_DELETE ] )
    {
        if ( has_sel )
        {
            medit_erase( buf, &len, es, sel_lo, sel_hi );
            medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
            changed = true;
        }
        else if ( es->cursor < len )
        {
            medit_erase( buf, &len, es, es->cursor, es->cursor + 1u );
            medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
            changed = true;
        }
        has_sel = false; sel_lo = sel_hi = es->cursor;
        blink = true;
        es->pref_valid = 0;
    }

    /* Enter: replace the selection with a newline.  A non-char edit, so it breaks the undo
       char group like paste / delete do. */
    if ( s_io.keys_pressed_repeat[ APP_KEY_ENTER ] )
    {
        bool did = false;
        if ( has_sel )
        {
            medit_erase( buf, &len, es, sel_lo, sel_hi );
            has_sel = false; sel_lo = sel_hi = es->cursor;
            did = true;
        }
        if ( medit_insert( buf, bufsz, &len, es, '\n' ) ) did = true;
        if ( did )
        {
            medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
            changed = true;
        }
        blink = true;
        es->pref_valid = 0;
    }

    /* Character input: replace the selection with the first incoming char, insert the rest at
       the advancing caret.  Control bytes are rejected here exactly like the single-line field
       (Enter's '\r' echo in the text stream must not double-insert the newline). */
    for ( const char* ch = ctrl ? "" : s_io.text; *ch; ++ch )
    {
        if ( (u8)*ch < 0x20u || (u8)*ch == 0x7Fu ) continue;

        if ( has_sel )
        {
            s_medit_undo.last_was_char = false;
            medit_erase( buf, &len, es, sel_lo, sel_hi );
            has_sel = false; sel_lo = sel_hi = es->cursor;
        }
        if ( !medit_insert( buf, bufsz, &len, es, *ch ) ) break;
        changed = true;
        blink   = true;
        es->pref_valid = 0;
    }

    /* Char-burst undo grouping: push on the first char of a burst, update in place after. */
    if ( changed && !ctrl && s_io.text[ 0 ] )
    {
        if ( !s_medit_undo.last_was_char )
        {
            medit_undo_push( &s_medit_undo, buf, es->cursor, es->anchor );
            s_medit_undo.last_was_char = true;
        }
        else
        {
            medit_undo_update( &s_medit_undo, buf, es->cursor, es->anchor );
        }
    }

    /* Escape reverts to the focus-gain content (when the revert copy fit) and drops focus. */
    if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
    {
        if ( s_medit_undo.revert_ok )
        {
            u32 rv_len = edit_strlen( s_medit_undo.revert, bufsz );
            if ( rv_len != len || memcmp( buf, s_medit_undo.revert, rv_len ) != 0 )
            {
                memcpy( buf, s_medit_undo.revert, rv_len + 1u );
                es->cursor = es->anchor = 0;
                changed = true;
                len     = rv_len;
            }
        }
        s_medit_undo.for_id = GUI_ID_NONE;
        focus_release();
    }

    *len_io     = len;
    *changed_io = changed;
    *blink_io   = blink;
}

/*==============================================================================================
    Mouse -- 2D click-to-caret, Shift-extend, double-click word select, drag select.

    `inner` is the text content rect (the canvas cell already inset by the widget, and offset by
    the region scroll), so the mapping is plain rect-local math plus the horizontal pan.  The same
    capture model as the single-line field: st.active holds the drag past the box edges, and
    medit_offset_at clamps the out-of-range row / x, so sweeping above or below the box extends
    the selection line by line (the caret chase then scrolls it into view).
==============================================================================================*/

static void
medit_apply_mouse( gui_rect_t inner, gui_item_state_t st, char* buf, u32 len,
                   gui_medit_state_t* es, bool shift, f32 line_h, bool* blink_io )
{
    if ( !( st.pressed || st.active ) ) return;

    f32 px  = s_io.mouse_x - inner.x + es->scroll_x;
    f32 py  = s_io.mouse_y - inner.y;
    i32 row = ( py < 0.0f ) ? -1 : (i32)( py / line_h );
    u32 off = medit_offset_at( buf, len, row, px );

    if ( st.pressed && s_io.mouse_double[ 0 ] )
    {
        /* Double-click: select the word under the cursor (shared right-edge correction). */
        u32 wb_off = word_click_off( buf, len, off );
        u32 wlo, whi;
        word_bounds( buf, len, wb_off, &wlo, &whi );
        es->anchor   = wlo;
        es->cursor   = whi;
        es->dbl_lo   = wlo;
        es->dbl_hi   = whi;
        es->word_sel = 1;
    }
    else if ( st.pressed )
    {
        es->cursor   = off;
        es->word_sel = 0;
        if ( !shift ) es->anchor = off;
    }
    else if ( st.active )
    {
        if ( es->word_sel )
        {
            /* Word-select drag: pin the double-clicked word, extend by word boundaries. */
            u32 drag_off = word_click_off( buf, len, off );
            if ( drag_off < es->dbl_lo )
            {
                u32 wlo, whi;
                word_bounds( buf, len, drag_off, &wlo, &whi );
                es->anchor = es->dbl_hi;
                es->cursor = wlo;
            }
            else if ( drag_off >= es->dbl_hi )
            {
                u32 wlo, whi;
                word_bounds( buf, len, drag_off, &wlo, &whi );
                es->anchor = es->dbl_lo;
                es->cursor = whi;
            }
            else
            {
                es->anchor = es->dbl_lo;
                es->cursor = es->dbl_hi;
            }
        }
        else
        {
            es->cursor = off;
        }
    }

    es->pref_valid = 0;
    *blink_io      = true;
}

/*==============================================================================================
    Horizontal caret chase -- pan the content within the cell so the caret stays in view, every
    frame like the single-line field (the vertical chase is the region's, owned by the widget).
    inner.w is the visible text width (the box already inset by the widget).
==============================================================================================*/

static void
medit_hscroll( gui_rect_t inner, const char* buf, gui_medit_state_t* es )
{
    u32 crow; f32 cx;
    medit_caret_rowx( buf, es->cursor, &crow, &cx );
    f32 vis_w = inner.w;
    if ( vis_w < 0.0f ) vis_w = 0.0f;
    if ( cx - es->scroll_x < 0.0f )  es->scroll_x = cx;
    if ( cx - es->scroll_x > vis_w ) es->scroll_x = cx - vis_w;
}

/*==============================================================================================
    medit_edit -- the one convenient entry: a full field-internal frame of a multiline editor.

    Fetches the editor's persisted state, requests the I-beam over the content, and -- while
    focused -- initialises the undo ring on first focus, applies every key command, then the mouse
    selection drag, then advances the caret-blink clock.  Every frame it pans the caret into view
    horizontally and reports any change to the item record.  Returns { changed, active }: `active`
    (any caret / edit activity this frame) tells the widget when to chase the caret vertically by
    scrolling the enclosing region -- the one thing an interact mechanism may not reach.  Leaves
    caret / anchor / scroll_x / blink on the keyed state slot for the widget to chase and paint;
    `inner` is the text content rect (the widget's cell already inset), `vis_rows` the page size.
==============================================================================================*/
medit_result_t
medit_edit( gui_id_t id, gui_rect_t inner, gui_item_state_t st, u32 vis_rows, f32 line_h,
            char* buf, u32 bufsz )
{
    gui_medit_state_t* es  = GUI_STATE( gui_medit_state_t, id );
    u32                len = edit_strlen( buf, bufsz );
    medit_result_t     r   = { false, false };

    /* I-beam over the text area, held through a selection drag (st.active). */
    if ( st.hover || st.active )
        cursor_set( APP_CURSOR_TEXT );

    /* Clamp caret and anchor -- a programmatic buffer change may have shortened the string. */
    if ( es->cursor > len ) es->cursor = len;
    if ( es->anchor > len ) es->anchor = len;

    if ( st.focused )
    {
        bool changed = false;
        bool active  = false;

        if ( s_medit_undo.for_id != id )
            medit_undo_init( &s_medit_undo, id, buf, es->cursor, es->anchor );

        medit_apply_keys( buf, bufsz, es, io_ctrl(), io_shift(), vis_rows, &len, &changed, &active );
        medit_apply_mouse( inner, st, buf, len, es, io_shift(), line_h, &active );

        /* Caret blink clock: any activity this frame un-hides the caret; otherwise it advances. */
        if ( active ) es->blink_t  = 0.0f;
        else          es->blink_t += s_io.dt;

        r.changed = changed;
        r.active  = active;
    }

    /* Pan the caret into view horizontally every frame so a programmatic move is honoured. */
    medit_hscroll( inner, buf, es );

    /* Report the edit to the item record (is_item_deactivated_after_edit, core/gui_query.c). */
    if ( r.changed )
        item_mark_edited();

    return r;
}

// clang-format on
/*============================================================================================*/
