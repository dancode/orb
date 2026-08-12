/*==============================================================================================

    runtime_service/gui/core/gui_io.c -- App input -> gui IO snapshot.

    Input reaches gui two ways, and the frame is bracketed around both:

    gui_event()      -- during the host's ring drain (BEFORE frame_begin): writes the
                        EVENT-borne input (typed text, wheel, clipboard paste) straight into
                        s_io, accumulating across the several events of one frame.

    io_frame_begin() -- from gui_frame_begin: samples the POLLED input (mouse position +
                        buttons, per-key state, display size) and computes s_io_dirty.  
                        Never touches the event-borne fields -- the drain already filled them.

    io_frame_end()   -- from gui_frame_end: clears the one-frame fields (text/wheel/paste) so
                        each is non-empty only on the frame its event arrived.
    
    The app event ring is single-consumer: the host drains it and hands each event here, so gui
    never drains it itself.  
    
    Because that drain completes before frame_begin, one storage slot per field suffices 
    -- gui_event fills it, widgets read it, io_frame_end clears it -- with no pending buffer.
    The result is s_io, the one snapshot every tier above it reads.

==============================================================================================*/

/* Compile-time check: GUI_KEY_COUNT must be large enough to index all app keys. */
ORB_STATIC_ASSERT( APP_KEY_COUNT <= GUI_KEY_COUNT,
                   "GUI_KEY_COUNT too small for APP_KEY_COUNT" );

// clang-format off
/*==============================================================================================
    State
==============================================================================================*/

/* The per-frame input snapshot the widgets see.  The polled fields are sampled by
   io_frame_begin(); the event-borne fields (text/wheel/paste) are written straight in by
   gui_event() during the host's ring drain and cleared by io_frame_end(). */
gui_io_t s_io;   /* extern'd in core/gui_core.h for the carved units */

/* True when any input-state change was detected this frame: mouse moved, button edge,
   key press/release, wheel, text, paste, or display-size change.  Computed in io_frame_begin
   and cleared at the next call.  Read by frame_begin to gate the frontend-dirty check. */
static bool s_io_dirty;

/* Set via viewport_mark_dirty() by gui_owned_window_event (frame/gui_viewport.c, across the
   unit seam) when a floater OS window is resized.  Consumed and cleared by io_frame_begin so
   the resize marks one frame dirty. */
static bool s_viewport_dirty;

void viewport_mark_dirty( void ) { s_viewport_dirty = true; }

/* Previous primary display size -- compared each frame to detect host-side viewport_resize
   calls on the main surface (which also change win_w/win_h passed into io_frame_begin). */
static i32 s_prev_disp_w, s_prev_disp_h;

/* Bookkeeping for the event-borne fields gui_event writes directly into s_io.  s_io_text_len is
   the append cursor into s_io.text (where the next typed char lands, and the bound check);
   s_io_paste_set records that a paste arrived this frame so an empty-string paste is distinguished
   from no paste.  Both reset by io_frame_end. */
static u32  s_io_text_len;
static bool s_io_paste_set;

/* The OS window the cursor is currently in, learned from the win_id on mouse move/button/wheel
   events (the polled position alone carries no window identity).  Win32 holds mouse capture on the
   origin window while a button is down, so during a drag these events keep arriving from that
   same window even if the cursor leaves it.  Cleared to invalid on up so it re-learns. */
static i32  s_pending_mouse_win = APP_WIN_INVALID;
static bool s_pending_mouse_win_set;

/* Double-click detection.  gui has no clock of its own, so the second press of a pair is
   recognised from the dt fed to frame_begin: a press counts as a double-click when it lands
   within DOUBLE_CLICK_TIME seconds of the previous press and within DOUBLE_CLICK_DIST pixels.
   s_click_elapsed grows by dt each frame and resets on every fresh press. */
#define DOUBLE_CLICK_TIME  0.30f    /* seconds between the two presses */
#define DOUBLE_CLICK_DIST  6.0f     /* max cursor travel between them (pixels) */

static f32 s_click_elapsed[ 3 ] = { 1.0e9f, 1.0e9f, 1.0e9f };   /* start "long ago" */
static f32 s_click_x[ 3 ], s_click_y[ 3 ];

/*==============================================================================================
    Snapshot readers -- the queries later tiers ask of s_io, named once.
==============================================================================================*/

/* Accessor for gui_frame_loop.c's frontend-dirty gate (frame unit, across the seam). */
bool io_dirty( void ) { return s_io_dirty; }

/* Modifier key helpers: poll both L and R variants so callers need not repeat the pair. */
bool io_ctrl ( void ) { return s_io.keys_down[ APP_KEY_LCTRL  ] || s_io.keys_down[ APP_KEY_RCTRL  ]; }
bool io_shift( void ) { return s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ]; }
bool io_alt  ( void ) { return s_io.keys_down[ APP_KEY_LALT   ] || s_io.keys_down[ APP_KEY_RALT   ]; }

/* Claim a key edge for this frame -- the one choke point every consumer (item activation, nav
   type-ahead, mnemonics) routes through instead of hand-zeroing s_io at its own call site.  Zeroes
   both the initial-press and repeat edges, so nothing later in the frame sees the key either way;
   keys_down / keys_released stay untouched on purpose -- a claim silences "was this just pressed",
   not "is it physically held".  Returns whether there was a live edge to take, so a caller can
   tell "I used it" from "there was nothing there".  This is tier 2 of the keyboard routing model
   laid out at gui_want_capture_keyboard (core/gui_query.c). */
bool
key_claim( app_key_t k )
{
    bool had_edge = s_io.keys_pressed[ k ] || s_io.keys_pressed_repeat[ k ];
    s_io.keys_pressed[ k ]        = false;
    s_io.keys_pressed_repeat[ k ] = false;
    return had_edge;
}

/*==============================================================================================
    Event intake -- gui_event() and the io_add_* feeders it unpacks the app ring through, running
    during the host's drain, before frame_begin.  Not part of the public widget API.

    The clipboard passes through here too, in both directions, because gui owns no clipboard
    buffer of its own -- it is a pure conduit between the OS and the focused field.  Outbound
    (cut / copy) goes straight out via app()->clipboard_set, so other applications see it;
    inbound (paste) arrives as an APP_EV_CLIPBOARD event that lands in s_io.paste for the focused
    field to consume this frame, cleared by io_frame_end after.
==============================================================================================*/

/* Copy n bytes of `s` to the OS clipboard, dropping control characters EXCEPT '\n' and '\t' --
   a multiline selection legitimately carries both and must round-trip with its line breaks
   (a single-line selection never contains either, so it is unaffected).  UTF-8 passes through
   untouched: every byte of a sequence is >= 0x80.  Builds a NUL-terminated temporary because
   the source is a slice of a larger buffer. */
void
gui_clipboard_set( const char* s, u32 n )
{
    char tmp[ sizeof( ( (gui_io_t*)0 )->paste ) ];
    u32  w = 0;
    for ( u32 i = 0; i < n && w + 1u < sizeof( tmp ); ++i )
        if ( (u8)s[ i ] >= 0x20u ? (u8)s[ i ] != 0x7Fu : ( s[ i ] == '\n' || s[ i ] == '\t' ) )
            tmp[ w++ ] = s[ i ];
    tmp[ w ] = '\0';
    app()->clipboard_set( tmp );
}

/* Write pasted text arriving via APP_EV_CLIPBOARD straight into s_io.paste for the focused field
   to consume this frame; io_frame_end clears it after.  s_io_paste_set marks that a paste
   happened so an empty-string paste is not mistaken for no paste.  The cap may cut an oversize
   paste mid-UTF-8-sequence; that is tolerated -- the edit engines decode-step the buffer and
   drop a trailing partial sequence as malformed, so nothing invalid reaches a field. */
static void
io_add_paste( const char* text )
{
    u32 i = 0;
    if ( text )
        for ( ; text[ i ] && i + 1u < sizeof( s_io.paste ); ++i )
            s_io.paste[ i ] = text[ i ];
    s_io.paste[ i ]  = '\0';
    s_io_paste_set   = true;
}

static void
io_add_char( u32 codepoint )
{
    /* Ignore control characters.  Windows posts WM_CHAR for backspace (0x08), tab,
       enter, escape, DEL (0x7F) etc.; those are handled via key state, not inserted
       as text -- without this, backspace would append '\b' that its own delete then
       removes, so it appears to do nothing. */
    if ( codepoint < 0x20u || codepoint == 0x7Fu )
        return;

    /* Encode into the frame text as UTF-8 -- the engine's strings are UTF-8 bytes, and the
       platform already delivered a whole UTF-32 codepoint (surrogates paired at the WM_CHAR
       seam).  utf8_encode returns 0 for an unpaired half or out-of-range value: drop.  Whole
       sequences only -- a full buffer rejects the entire character, never a partial byte. */
    char seq[ UTF8_MAX_BYTES ];
    u32  nb = utf8_encode( codepoint, seq );
    if ( nb == 0u )
        return;
    if ( s_io_text_len + nb < sizeof( s_io.text ) )
    {
        for ( u32 i = 0; i < nb; ++i )
            s_io.text[ s_io_text_len++ ] = seq[ i ];
        s_io.text[ s_io_text_len ] = '\0';
    }
}

static void
io_add_wheel( f32 delta )
{
    s_io.mouse_wheel += delta;
}

/*==============================================================================================
    GUI Event -- the host's ring drain calls this for every event, before frame_begin.   

    Forward one drained app event to gui.  The host loops its event ring and
    passes every event here; gui unpacks the input events it cares about (text +
    scroll) so that logic lives in one place instead of in every host's switch.

    Answers the app_event_result_t routing schema (app.h): PASS = untouched, SHARED = acted on
    but the event is broadcast, CONSUMED = gui owns it and routing stops.  Hosts need no
    per-event-type knowledge -- the whole rule is
    `if ( gui()->event( &ev ) == APP_EVENT_CONSUMED ) continue;`.

==============================================================================================*/

app_event_result_t
gui_event( const app_event_t* ev )
{
    switch ( ev->type )
    {
        case APP_EV_CHAR:
            io_add_char( ev->data.text.codepoint );
            return APP_EVENT_CONSUMED;

        case APP_EV_MOUSE_WHEEL:
            s_pending_mouse_win     = ev->win_id;   /* route the wheel to the cursor's surface */
            s_pending_mouse_win_set = true;
            io_add_wheel( (f32)ev->data.mouse_wheel.delta );
            return APP_EVENT_CONSUMED;

        case APP_EV_CLIPBOARD:
            io_add_paste( ev->data.clipboard.text );
            return APP_EVENT_CONSUMED;

        /* Key state is polled, not event-borne: io_frame_begin samples every key into s_io each
           frame, and consumers read it through the tier model (key_claim / is_key_pressed).  So key
           events need no handling here -- including the debug layer hotkeys (NP1-NP5), which now live
           with the rest of the debug driver in debug_hotkeys (frame/gui_frame_overlay.c) on the same
           polled channel as F9/F10/P/O, instead of this separate event-time path.  Falls through to
           the default (PASS), so an unbound F-key still reaches the host's bind system. */

        /* Position + buttons are still resolved by io_frame_begin from the polled snapshot (client
           coords of the window the cursor is in); these events carry the win_id that identifies
           WHICH window that is, so the host viewport can be resolved.  SHARED, not CONSUMED -- the
           mouse-capture fence (want_capture_mouse) decides UI-vs-scene at read time, not here. */
        case APP_EV_MOUSE_MOVE:
        case APP_EV_MOUSE_DOWN:
        case APP_EV_MOUSE_UP:
            s_pending_mouse_win     = ev->win_id;
            s_pending_mouse_win_set = true;
            return APP_EVENT_SHARED;

        /* A gui-OWNED floater's OS window resize/close is gui's to service (it owns that
           window + rhi context).  Delegate to the viewport-pool helper, which grades the event
           against the pool: CONSUMED for an owned surface, SHARED for the host's primary
           surface gui only tracks, PASS for a window gui knows nothing about. */
        case APP_EV_WIN_RESIZE:
        case APP_EV_WIN_CLOSE:
            return gui_owned_window_event( ev );

        default:
            return APP_EVENT_PASS;
    }
}

/*==============================================================================================
    Input frame lifecycle -- io_frame_begin / io_frame_end, bracketing the widgets' reads.
==============================================================================================*/

/* Per-button double-click detection: a press counts as a double-click when it lands within
   DOUBLE_CLICK_TIME of the previous press and within DOUBLE_CLICK_DIST of it.  s_click_elapsed
   grows by dt each frame (gui has no clock of its own) and resets on every fresh press; a detected
   double consumes the timer so a third press is a fresh single.  Writes s_io.mouse_double[]. */
static void
io_detect_double_click( f32 dt )
{
    for ( u32 i = 0; i < 3; ++i )
    {
        s_io.mouse_double[ i ] = false;
        s_click_elapsed[ i ]  += dt;

        if ( s_io.mouse_pressed[ i ] )
        {
            f32  dx      = s_io.mouse_x - s_click_x[ i ];
            f32  dy      = s_io.mouse_y - s_click_y[ i ];
            bool in_time = s_click_elapsed[ i ] <= DOUBLE_CLICK_TIME;
            bool in_dist = ( dx * dx + dy * dy ) <= DOUBLE_CLICK_DIST * DOUBLE_CLICK_DIST;

            if ( in_time && in_dist )
            {
                s_io.mouse_double[ i ] = true;
                s_click_elapsed[ i ]   = 1.0e9f;   /* consume: a 3rd press is a fresh single */
            }
            else
            {
                s_click_elapsed[ i ] = 0.0f;       /* first press of a potential pair */
            }
            s_click_x[ i ] = s_io.mouse_x;
            s_click_y[ i ] = s_io.mouse_y;
        }
    }
}

/* io_frame_begin -- sample the polled input into s_io for the current frame.

   The head of the input frame, called from gui_frame_begin.  Samples what must be polled (mouse
   position + buttons, per-key state, display size) and computes s_io_dirty.  The event-borne
   fields (text/wheel/paste) are NOT touched here -- gui_event already wrote them straight into
   s_io during the host's ring drain, which runs before frame_begin; io_frame_end clears them. */

void
io_frame_begin( i32 win_w, i32 win_h, f32 dt )
{
    /* Mouse position (polled): client coords of the window the cursor is in.
       Compare against the previous frame before overwriting to detect movement. */
    bool mouse_moved;
    {
        i32 mx = 0, my = 0;
        app()->mouse_position( &mx, &my );
        mouse_moved   = ( mx != (i32)s_io.mouse_x || my != (i32)s_io.mouse_y );
        s_io.mouse_x  = (f32)mx;
        s_io.mouse_y  = (f32)my;
    }

    /* Resolve the surface the cursor is in from the most recent mouse event's win_id.  Only when a
       mouse event actually arrived this frame -- otherwise the cursor has not crossed to another
       window, so the last resolved viewport still holds (s_io persists across frames). */
    if ( s_pending_mouse_win_set )
    {
        s_io.mouse_viewport     = viewport_index_for_window( s_pending_mouse_win );
        s_pending_mouse_win_set = false;
    }

    /* Mouse button snapshot (left=0, right=1, middle=2).
       Any pressed or released edge makes the frame dirty. */
    bool mouse_edge = false;
    {
        const app_mouse_button_t map[ 3 ] = {
            APP_MOUSE_LEFT, APP_MOUSE_RIGHT, APP_MOUSE_MIDDLE
        };
        for ( u32 i = 0; i < 3; ++i )
        {
            s_io.mouse_down     [ i ] = app()->mouse_button_down     ( map[ i ] );
            s_io.mouse_pressed  [ i ] = app()->mouse_button_pressed  ( map[ i ] );
            s_io.mouse_released [ i ] = app()->mouse_button_released ( map[ i ] );
            if ( s_io.mouse_pressed[ i ] || s_io.mouse_released[ i ] ) mouse_edge = true;
        }
    }

    /* Double-click: a press soon after, and close to, the previous press. */
    io_detect_double_click( dt );

    /* Key state snapshot.  keys_pressed is the initial press; keys_pressed_repeat also catches OS
       auto-repeat ticks (held backspace / arrows in a text field), the caller picking which it reads.
       Fold any press, release, or repeat tick into key_edge while we scan -- free since we scan anyway. */
    bool key_edge = false;
    for ( i32 k = 0; k < APP_KEY_COUNT; ++k )
    {
        s_io.keys_down           [ k ] = app()->key_down           ( (app_key_t)k );
        s_io.keys_pressed        [ k ] = app()->key_pressed        ( (app_key_t)k );
        s_io.keys_pressed_repeat [ k ] = app()->key_pressed_repeat ( (app_key_t)k );
        s_io.keys_released       [ k ] = app()->key_released       ( (app_key_t)k );
        if ( s_io.keys_pressed[ k ] || s_io.keys_released[ k ] || s_io.keys_pressed_repeat[ k ] ) key_edge = true;
    }

    /* Text, scroll and paste were written straight into s_io by gui_event during the host's ring
       drain, which completes before this call -- nothing to promote here.  They stay live for the
       widgets this frame and are cleared by io_frame_end. */

    /* Display size change: primary window resized (win_w/win_h changed) or a floater viewport
       was resized (s_viewport_dirty set by gui_owned_window_event).  Either invalidates the
       cached layout -- window clip rects and draw_reset dimensions must be recomputed. */
    bool disp_changed = ( win_w != s_prev_disp_w || win_h != s_prev_disp_h ) || s_viewport_dirty;
    s_prev_disp_w   = win_w;
    s_prev_disp_h   = win_h;
    s_viewport_dirty = false;

    /* Frame is dirty when anything changed vs last frame: position, button/key edges (including
       repeat ticks), wheel, typed text, clipboard paste, or display-size change.  The event-borne
       fields are read from s_io, where gui_event just left them. */
    s_io_dirty = mouse_moved || mouse_edge || key_edge || disp_changed
              || ( s_io.mouse_wheel != 0.0f )
              || ( s_io_text_len    >  0    )
              || s_io_paste_set;

    s_io.display_w = win_w;
    s_io.display_h = win_h;
    s_io.dt        = dt;
    s_io.time     += (f64)dt;   /* monotonic frame clock for get_time() */
}

/* io_frame_end -- clear the one-frame event-borne input.

   The tail of the input frame, called from gui_frame_end unconditionally (every frame, including
   clean / idle-skipped ones).  Text, wheel and paste are non-empty only on the frame their event
   arrived, so they are cleared here after the widgets have read them and before the next drain
   refills them.  Valid because the host drains the event ring (which fills these fields) before
   frame_begin -- never between this call and the next io_frame_begin. */

void
io_frame_end( void )
{
    s_io.text[ 0 ]   = '\0';
    s_io_text_len    = 0;
    s_io.mouse_wheel = 0.0f;
    s_io.paste[ 0 ]  = '\0';
    s_io_paste_set   = false;
}

// clang-format on
/*============================================================================================*/
