/*==============================================================================================

    runtime_service/gui/widgets/gui_text_edit_multi.c -- Multi-line text editor widget.

    input_text_multiline (the Dear ImGui InputTextMultiline analogue): a text-area box for
    script bodies, notes, and console transcripts.  Enter inserts a newline (it never submits
    or drops focus -- Escape reverts and leaves, exactly like the single-line field), the
    caret moves in two dimensions (Up / Down / PageUp / PageDown with a sticky preferred
    column), Home / End are line-local with Ctrl jumping to the buffer ends, and selection /
    clipboard / undo mirror the single-line engine byte-for-byte where the semantics agree.

    Layout: one field-label row of caller-chosen height (0 = eight lines); the box scrolls
    internally in both axes rather than growing.  No word wrap (like Dear ImGui): long lines
    pan horizontally, chasing the caret the same way the single-line field does.

    Vertical scroll is LINE-SNAPPED: there is no vertical text clip primitive, so a row is
    drawn fully inside the box interior or not at all, and snapping the scroll to whole lines
    keeps that from reading as pop-in (the terminal model).  Horizontal overflow reuses the
    glyph-level clip window (draw_push_text_clip_n) per row, so the widget never opens a
    scissor and stays merged in the window batch -- the self-fit-over-clips rule.  When the
    content overflows vertically a scrollbar_widget rides in a reserved gutter at the right
    edge (inside the box, outside the text hit rect), and the wheel scrolls the hovered box
    through the same innermost-wins claim the region engine uses (s_build.wheel_used).

    Undo / redo is a private ring (the single-line ring is 256 bytes -- too small here) with
    the same shape: snapshots after each committed edit, char-burst grouping, Escape-revert
    copy taken at focus gain.  A buffer too large for a snapshot slot marks the history dead
    until the next focus gain, so undo never restores a truncated buffer.

    Per-id persisted state (caret, anchor, both scrolls, preferred column, blink) rents a
    big-class slot from the keyed state pool.  Included by gui.c after widgets/gui_input.c so
    the single-line engine's byte-offset helpers (char_class, word_bounds, word_click_off,
    edit_strlen, text_x_at, text_offset_at), scrollbar_widget, and the paint helpers are all
    in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Multiline edit state -- persisted per-id across frames (big-class keyed state slot).

    cursor / anchor are byte offsets with the same selection contract as the single-line field:
    [min,max) highlighted, equal means a bare caret.  scroll_x chases the caret; scroll_y is
    line-snapped and owned jointly by the caret chase, the wheel, and the gutter scrollbar.
    pref_x is the sticky preferred column for vertical caret movement (the x the caret aims for
    when Up / Down crosses a shorter line); pref_valid gates it because 0.0 is a real column.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32  blink_t;      /* seconds since last caret-visibility reset */
    u32  cursor;       /* byte offset of the caret */
    u32  anchor;       /* passive end of the selection; cursor == anchor -> none */
    u32  dbl_lo;       /* word start of the double-clicked word (word-drag mode) */
    u32  dbl_hi;       /* word end of the double-clicked word  (word-drag mode)  */
    f32  scroll_x;     /* horizontal pixel scroll (caret chase) */
    f32  scroll_y;     /* vertical pixel scroll, snapped to whole lines */
    f32  pref_x;       /* preferred caret column (pixels) for vertical movement */
    u8   word_sel;     /* nonzero while in word-select drag (set by double-click) */
    u8   pref_valid;   /* pref_x holds a live column (0.0 is a real column, so a flag) */
    u8   _pad[ 2 ];

} gui_medit_state_t;
/* 40 bytes -- big-class tenant (GUI_STATE_BIG_CAP). */

/* Selection bounds from the caret/anchor pair (u32 twin of edit_sel). */
static void
medit_sel( const gui_medit_state_t* es, u32* lo, u32* hi, bool* has )
{
    *lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
    *hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
    *has = ( *lo != *hi );
}

/*----------------------------------------------------------------------------------------------
    Line geometry -- byte-offset <-> (row, pixel-x) mapping over the '\n'-separated buffer.

    All linear scans: an editor-sized buffer (a few KB) rescans in negligible time, and the
    scans only run on frames with caret or paint activity for the focused / visible field.
----------------------------------------------------------------------------------------------*/

/* Number of display lines: '\n' count + 1 (an empty buffer is one empty line). */
static u32
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
static u32
medit_line_end( const char* buf, u32 len, u32 off )
{
    while ( off < len && buf[ off ] != '\n' ) ++off;
    return off;
}

/* Start offset of display line `row`, clamped to the last line when row overshoots. */
static u32
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
static void
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

/*----------------------------------------------------------------------------------------------
    Undo / redo ring -- private multiline twin of the single-line ring (gui_text_edit.c).

    Same shape (snapshots after committed edits, char-burst grouping, Escape-revert copy at
    focus gain) with a bigger slot and one extra rule: a buffer that no longer fits a slot
    marks the whole history DEAD until the next focus gain.  Undo must never restore a
    truncated snapshot over a longer live buffer, so an oversize edit simply switches undo
    off rather than corrupting the text.  revert_ok gates Escape the same way.
----------------------------------------------------------------------------------------------*/

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

/*----------------------------------------------------------------------------------------------
    Edit primitives -- the two buffer mutations every path shares.
----------------------------------------------------------------------------------------------*/

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

/*----------------------------------------------------------------------------------------------
    Keyboard -- clipboard, undo / redo, 2D navigation, deletion, insertion, Escape-revert.

    Byte-for-byte the single-line contract where the semantics agree (Left / Right, word
    jumps, Ctrl+A, Backspace / Delete, char bursts); the multiline differences:
        - Enter inserts '\n' (repeats); it never submits or drops focus
        - Up / Down / PageUp / PageDown move the caret vertically (Shift extends)
        - Home / End are line-local; Ctrl+Home / Ctrl+End jump to the buffer ends
        - paste keeps newlines (CRLF / CR normalised to '\n', tabs expand to 4 spaces)
    char_class treats '\n' as whitespace, so the shared word ops cross lines naturally.
----------------------------------------------------------------------------------------------*/

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
        item_focus_release();
    }

    *len_io     = len;
    *changed_io = changed;
    *blink_io   = blink;
}

/*----------------------------------------------------------------------------------------------
    Mouse -- 2D click-to-caret, Shift-extend, double-click word select, drag select.

    The same capture model as the single-line field: st.active holds the drag past the box
    edges, and medit_offset_at clamps the out-of-range row / x, so sweeping above or below
    the box extends the selection line by line (the caret chase then scrolls it into view).
----------------------------------------------------------------------------------------------*/

static void
medit_apply_mouse( gui_rect_t box, gui_item_state_t st, char* buf, u32 len,
                   gui_medit_state_t* es, bool shift, f32 line_h, bool* blink_io )
{
    if ( !( st.pressed || st.active ) ) return;

    f32 px  = s_io.mouse_x - ( box.x + WIDGET_PAD ) + es->scroll_x;
    f32 py  = s_io.mouse_y - ( box.y + WIDGET_PAD ) + es->scroll_y;
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

/*----------------------------------------------------------------------------------------------
    Scroll + paint -- wheel / caret-chase / scrollbar folded into the line-snapped scroll_y,
    then per-row selection highlight, glyph-clipped text, and the blinking caret.

    `follow` is this frame's caret activity (any key or click): only then does the vertical
    chase run, so wheel and scrollbar scrolling are free to leave the caret off-screen.
----------------------------------------------------------------------------------------------*/

static void
medit_scroll_and_paint( gui_id_t id, gui_rect_t box, char* buf, u32 len, gui_medit_state_t* es,
                        gui_item_state_t st, bool focused, bool follow, bool vbar, u32 vis_rows )
{
    const f32 line_h = font_line_h();
    const f32 char_h = font_char_h();
    u32       nlines = medit_line_count( buf, len );

    f32 x0    = box.x + WIDGET_PAD;
    f32 y0    = box.y + WIDGET_PAD;
    f32 vis_w = box.w - 2.0f * WIDGET_PAD - ( vbar ? SLIDER_KNOB_W + WIN_BORDER : 0.0f );
    if ( vis_w < 0.0f ) vis_w = 0.0f;

    f32 view_h     = (f32)vis_rows * line_h;
    f32 max_scroll = ( nlines > vis_rows ) ? (f32)( nlines - vis_rows ) * line_h : 0.0f;

    /* Wheel over the box: the same innermost-wins claim the region engine uses, so the
       parent window does not also scroll. */
    if ( st.hover && !s_build.wheel_used && interact_idle() && s_io.mouse_wheel != 0.0f )
    {
        es->scroll_y      -= s_io.mouse_wheel * 3.0f * line_h;
        s_build.wheel_used = true;
    }

    /* Vertical caret chase, line-granular: scroll the caret's row fully into view. */
    u32 crow; f32 cx;
    medit_caret_rowx( buf, es->cursor, &crow, &cx );
    if ( follow )
    {
        if ( (f32)crow * line_h < es->scroll_y )
            es->scroll_y = (f32)crow * line_h;
        if ( (f32)( crow + 1u ) * line_h > es->scroll_y + view_h )
            es->scroll_y = (f32)( crow + 1u ) * line_h - view_h;
    }

    /* Gutter scrollbar (widgets/gui_scrollbar.c owns the grab and the paint). */
    if ( vbar )
    {
        gui_rect_t track = { box.x + box.w - WIN_BORDER - SLIDER_KNOB_W, box.y + WIN_BORDER,
                             SLIDER_KNOB_W, box.h - 2.0f * WIN_BORDER };
        scrollbar_widget( id, track, true, (f32)nlines * line_h, view_h, &es->scroll_y );
    }

    /* Clamp, then snap to whole lines: with no vertical clip primitive a row draws fully or
       not at all, and the snap keeps that from reading as pop-in.  max_scroll is a line
       multiple by construction, so the snap cannot push past it. */
    if ( es->scroll_y > max_scroll ) es->scroll_y = max_scroll;
    if ( es->scroll_y < 0.0f )       es->scroll_y = 0.0f;
    es->scroll_y = (f32)(u32)( es->scroll_y / line_h + 0.5f ) * line_h;

    /* Horizontal caret chase, every frame like the single-line field. */
    if ( cx - es->scroll_x < 0.0f )  es->scroll_x = cx;
    if ( cx - es->scroll_x > vis_w ) es->scroll_x = cx - vis_w;

    f32 text_x  = x0 - es->scroll_x;
    f32 clip_x0 = x0;
    f32 clip_x1 = x0 + vis_w;

    u32  sel_lo, sel_hi;
    bool has_sel;
    medit_sel( es, &sel_lo, &sel_hi, &has_sel );

    /* Row walk: visible lines only, each hard-cut to the horizontal clip window at emit time
       (no scissor, no batch split -- the single-line field's glyph clip, once per row). */
    u32 first = (u32)( es->scroll_y / line_h + 0.5f );
    u32 ls    = medit_row_start( buf, len, first );

    for ( u32 row = first; ; ++row )
    {
        f32 ry = y0 + (f32)( row - first ) * line_h;
        if ( ry + char_h > box.y + box.h - WIDGET_PAD + 0.5f ) break;

        u32 le = medit_line_end( buf, len, ls );

        /* Selection highlight for this row's slice of [sel_lo,sel_hi); a selection running
           past the row end covers its newline, shown as a one-space tail (an empty line
           inside the selection stays visible as just that tail). */
        if ( focused && has_sel && sel_lo <= le && sel_hi > ls )
        {
            u32 a   = sel_lo > ls ? sel_lo : ls;
            u32 b   = sel_hi < le ? sel_hi : le;
            f32 sx0 = text_x + text_x_at( buf + ls, a - ls );
            f32 sx1 = text_x + text_x_at( buf + ls, b - ls );
            if ( sel_hi > le && le < len ) sx1 += font_char_advance( ' ' );
            if ( sx0 < clip_x0 ) sx0 = clip_x0;
            if ( sx1 > clip_x1 ) sx1 = clip_x1;
            if ( sx1 > sx0 )
                draw_fill( ( gui_rect_t ){ sx0, ry - 1.0f, sx1 - sx0, char_h + 2.0f },
                           COL_WIDGET_ACT );
        }

        if ( le > ls )
            draw_push_text_clip_n( text_x, ry, COL_TEXT, buf + ls, le - ls, clip_x0, clip_x1 );

        /* Blinking caret on its row (visible for the first 0.5 s of each 1 s cycle). */
        if ( focused && es->cursor >= ls && es->cursor <= le )
        {
            bool caret_vis = ( ( (u32)( es->blink_t * 2.0f ) ) & 1u ) == 0u;
            f32  cxp       = text_x + text_x_at( buf + ls, es->cursor - ls );
            if ( caret_vis && cxp >= clip_x0 - 0.5f && cxp <= clip_x1 + 0.5f )
                draw_fill( ( gui_rect_t ){ cxp, ry, (f32)s_style.cursor_w, char_h },
                           COL_CURSOR );
        }

        if ( le >= len ) break;
        ls = le + 1u;
    }
}

/*----------------------------------------------------------------------------------------------
    medit_field_edit -- the multiline engine over a caller-carved box.

    Owns the item claim (hit rect excludes the scrollbar gutter so a bar press never seats
    the caret), the box frame draw, and the keyboard / mouse / scroll / paint sequence.
    Returns true on any buffer modification this frame.
----------------------------------------------------------------------------------------------*/

static bool
medit_field_edit( gui_id_t id, gui_rect_t box, char* buf, u32 bufsz )
{
    gui_medit_state_t* es  = GUI_STATE( gui_medit_state_t, id );
    u32                len = edit_strlen( buf, bufsz );

    const f32 line_h = font_line_h();
    f32       vis_h  = box.h - 2.0f * WIDGET_PAD;
    u32       vis_rows = (u32)( vis_h / line_h );
    if ( vis_rows < 1u ) vis_rows = 1u;

    /* Gutter decision from this frame's pre-edit content (an overflowing edit shows the bar
       next frame -- the same one-frame settle the region gutters have). */
    bool vbar = (f32)medit_line_count( buf, len ) * line_h > vis_h + 0.5f;

    gui_rect_t hit = box;
    if ( vbar ) hit.w -= SLIDER_KNOB_W + WIN_BORDER;

    gui_item_state_t st = item_state( id, hit, ITEM_FOCUSABLE );

    draw_fill( box, st.focused ? COL_INPUT_FOCUS : col_frame_bg( st, COL_INPUT_BG ) );
    draw_outline( box, WIN_BORDER, st.focused ? COL_WIDGET_HOT : COL_BORDER );

    /* I-beam over the text area, held through a selection drag (st.active). */
    if ( st.hover || st.active )
        cursor_set( APP_CURSOR_TEXT );

    /* Clamp caret and anchor -- a programmatic buffer change may have shortened the string. */
    if ( es->cursor > len ) es->cursor = len;
    if ( es->anchor > len ) es->anchor = len;

    bool changed     = false;
    bool blink_reset = false;

    if ( st.focused )
    {
        if ( s_medit_undo.for_id != id )
            medit_undo_init( &s_medit_undo, id, buf, es->cursor, es->anchor );

        bool shift = io_shift();
        bool ctrl  = io_ctrl();

        medit_apply_keys( buf, bufsz, es, ctrl, shift, vis_rows, &len, &changed, &blink_reset );
        medit_apply_mouse( box, st, buf, len, es, shift, line_h, &blink_reset );

        if ( blink_reset ) es->blink_t = 0.0f;
        else               es->blink_t += s_io.dt;
    }

    medit_scroll_and_paint( id, box, buf, len, es, st, st.focused, blink_reset, vbar, vis_rows );

    /* Accumulate the edit flag for is_item_deactivated_after_edit (user/gui_query.c). */
    if ( changed )
        item_mark_edited();

    return changed;
}

/*----------------------------------------------------------------------------------------------
    input_text_multiline -- public entry: label split + box carve over the engine above.
----------------------------------------------------------------------------------------------*/

bool
gui_input_text_multiline( const char* label, char* buf, u32 bufsz, f32 h )
{
    if ( h <= 0.0f )
        h = font_line_h() * 8.0f + 2.0f * WIDGET_PAD;

    gui_id_t   id    = item_id( label );
    gui_rect_t box_r = draw_field_label( cell_next( h ), label,
                                         font_char_h() * 3.0f, COL_TEXT_DIM );
    return medit_field_edit( id, box_r, buf, bufsz );
}

// clang-format on
/*============================================================================================*/
