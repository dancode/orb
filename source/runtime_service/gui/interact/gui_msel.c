/*==============================================================================================

    runtime_service/gui/interact/gui_msel.c -- multi-select protocol engine.

    The interaction protocol behind a multi-selected list (asset browsers, outliners, layer
    stacks): plain click replaces the selection, Ctrl+click toggles one item, Shift+click
    ranges from the anchor, Ctrl+Shift+click adds the range, Shift+arrow extends under
    keyboard nav, and Ctrl+A selects all.  A gesture mechanism like move / edit: it consumes
    (index, item state, io) and paints nothing.

    The engine does NOT store the selection -- the caller does (a bool array, a bitset, a
    component flag), because a list may be huge, virtualized (rows_clip emits only visible
    rows), or backed by external data.  Each frame the protocol resolves to ONE index-range
    action (gui_msel_t) the caller applies to its storage at end; gui_msel_apply is the
    ready-made application for the dense bool-array case.  Ranges are index math, so an
    action can span rows that were never emitted this frame.

    Scope bracket: gui_msel_begin(id, count) .. rows feed .. gui_msel_end() -> action.
    The stock row is chrome's gui_msel_item; a custom row (grid tile, tree line) is
    rect + item() + own paint + gui_msel_feed(index, st).

    The persistent state is small and keyed: the range ANCHOR (the last non-shift landing,
    Explorer's model -- shift never moves it) and the nav cursor's last row for edge-detecting
    keyboard arrival.  One scope can be live at a time (a static scratch bracket, like the
    layout stack's one-shots); nesting is not a list-selection shape.

    Included by gui_interact.c after the edit engines.

==============================================================================================*/

// clang-format off

/* Persisted per-list state (keyed slot, tiny class).  Both indices are stored +1 so the
   zeroed fresh slot reads as "unset" (0 is a real row index). */

typedef struct
{
    i32 anchor_p1;   // range anchor row + 1; 0 = no anchor yet (shift ranges from the row itself)
    i32 nav_p1;      // nav-cursor row + 1 as of last feed; 0 = cursor not on a row

} gui_msel_state_t;

/* The one live scope bracket (begin .. feeds .. end). */

typedef struct
{
    bool              open;      // inside a begin/end bracket
    bool              engaged;   // a row saw hover or the nav cursor -- gates the scope hotkeys
    i32               count;     // caller list length (ALL range, apply clamp)
    gui_msel_t        out;       // the frame's resolved action (last decider wins)
    gui_msel_state_t* st;        // keyed slot for the bracket's list id

} msel_scratch_t;

static msel_scratch_t s_msel;

/*==============================================================================================
    The click rule -- pure modifier math, shared by mouse clicks and keyboard activation
    (nav synthesizes clicks through item_state, so both arrive on the same seam).
==============================================================================================*/

/* Resolve one click on `index` with the live modifiers against the current anchor (-1 = none).
   Shift ranges from the anchor (Ctrl keeps the rest, plain replaces); Ctrl alone toggles the
   row; plain replaces with the row. */
gui_msel_t
msel_click_op( i32 anchor, i32 index, bool ctrl, bool shift )
{
    if ( shift )
    {
        i32 a  = ( anchor >= 0 ) ? anchor : index;
        i32 lo = ( a < index ) ? a : index;
        i32 hi = ( a < index ) ? index : a;
        return ( gui_msel_t ){ ctrl ? GUI_MSEL_ADD : GUI_MSEL_SET, lo, hi };
    }
    if ( ctrl )
        return ( gui_msel_t ){ GUI_MSEL_TOGGLE, index, index };
    return ( gui_msel_t ){ GUI_MSEL_SET, index, index };
}

/*==============================================================================================
    The scope bracket
==============================================================================================*/

/* Open the protocol scope for one list.  `id_str` keys the anchor state (scope-hashed like any
   widget id); `count` is the full list length -- pass it even when only a visible slice emits,
   it bounds Ctrl+A and the apply clamp. */
void
gui_msel_begin( const char* id_str, i32 count )
{
    s_msel.open    = true;
    s_msel.engaged = false;
    s_msel.count   = count;
    s_msel.out     = ( gui_msel_t ){ GUI_MSEL_NONE, 0, 0 };
    s_msel.st      = GUI_STATE( gui_msel_state_t, item_id( id_str ) );
}

/* Feed one row's resolved item state.  Call once per emitted row, any presentation.  Clicks
   run the click rule; the nav cursor ARRIVING on a row while Shift is held extends from the
   anchor (Shift+arrow) -- a plain arrival moves the cursor without touching the selection
   (selection follows on activation: Space / Enter synthesize a click). */
void
gui_msel_feed( i32 index, gui_item_state_t st )
{
    if ( !s_msel.open )
        return;

    gui_msel_state_t* ms = s_msel.st;

    if ( st.hover || st.nav )
        s_msel.engaged = true;

    if ( st.nav )
    {
        if ( ms->nav_p1 != index + 1 )   /* the cursor landed on this row this frame */
        {
            if ( io_shift() )
                s_msel.out = msel_click_op( ms->anchor_p1 - 1, index, io_ctrl(), true );
            ms->nav_p1 = index + 1;
        }
    }
    else if ( ms->nav_p1 == index + 1 )
        ms->nav_p1 = 0;                  /* cursor left; the row it moved to re-stamps */

    if ( st.clicked )
    {
        s_msel.out = msel_click_op( ms->anchor_p1 - 1, index, io_ctrl(), io_shift() );
        if ( !io_shift() )
            ms->anchor_p1 = index + 1;   /* shift never moves the anchor (Explorer model) */
    }
}

/* Close the scope and return the frame's action.  Ctrl+A resolves here -- scope-wide, so it
   only needs the bracket, not a row: gated on the scope being engaged (a row hovered or
   holding the nav cursor) and on no text field owning the keyboard.  Any action forces the
   next frame: it lands in caller storage this frame but is only visible at the next emit. */
gui_msel_t
gui_msel_end( void )
{
    if ( s_msel.open && s_msel.engaged
         && s_interaction.focused_id == GUI_ID_NONE
         && io_ctrl() && s_io.keys_pressed[ APP_KEY_A ] )
        s_msel.out = ( gui_msel_t ){ GUI_MSEL_ALL, 0, s_msel.count - 1 };

    if ( s_msel.out.op != GUI_MSEL_NONE )
        redraw_request();

    s_msel.open = false;
    return s_msel.out;
}

/*==============================================================================================
    Dense-array application -- the common storage, so most callers write no range code.
==============================================================================================*/

/* Apply an action to a bool-per-item array.  Pure; the range is INTERSECTED with [0, count) --
   a range fully outside goes empty (a SET then clears and selects nothing), never wraps onto
   a real row.  Callers with other storage (bitset, component flag) switch on act.op themselves. */
void
gui_msel_apply( gui_msel_t act, bool* sel, i32 count )
{
    if ( act.op == GUI_MSEL_NONE || !sel || count <= 0 )
        return;

    i32 lo = ( act.lo < 0 ) ? 0 : act.lo;
    i32 hi = ( act.hi >= count ) ? count - 1 : act.hi;

    switch ( act.op )
    {
        case GUI_MSEL_SET:
            for ( i32 i = 0; i < count; ++i )
                sel[ i ] = ( i >= lo && i <= hi );
            break;
        case GUI_MSEL_ADD:
            for ( i32 i = lo; i <= hi; ++i )
                sel[ i ] = true;
            break;
        case GUI_MSEL_TOGGLE:
            for ( i32 i = lo; i <= hi; ++i )
                sel[ i ] = !sel[ i ];
            break;
        case GUI_MSEL_ALL:
            for ( i32 i = 0; i < count; ++i )
                sel[ i ] = true;
            break;
        case GUI_MSEL_CLEAR:
            for ( i32 i = 0; i < count; ++i )
                sel[ i ] = false;
            break;
        default:
            break;
    }
}

// clang-format on
/*============================================================================================*/
