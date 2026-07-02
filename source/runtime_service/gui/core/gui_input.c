/*==============================================================================================

    runtime_service/gui/core/gui_input.c -- App input -> gui IO snapshot.

    input_update() is called once per frame before any widget calls.  It:
        1. Samples mouse position and button state via the app() query API.
        2. Samples per-key down/pressed state for all APP_KEY_COUNT keys.
        3. Consumes text + scroll fed in since the last frame by gui_event().
    The app event ring is single-consumer; the host drains it (for resize etc.) and
    hands each event to gui_event(), which unpacks CHAR / MOUSE_WHEEL internally,
    so gui does not drain the ring itself.  The result is stored in s_io and read
    by the widget code.

    Included by gui.c after gui_emit_draw.c.

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   /* gui_io_t, GUI_KEY_COUNT */
// clang-format off

/* The IO snapshot type (gui_io_t) and GUI_KEY_COUNT are defined in gui_internal.h. */

/* Compile-time check: GUI_KEY_COUNT must be large enough to index all app keys. */
ORB_STATIC_ASSERT( APP_KEY_COUNT <= GUI_KEY_COUNT,
                   "GUI_KEY_COUNT too small for APP_KEY_COUNT" );

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static gui_io_t s_io;

/* True when any input-state change was detected this frame: mouse moved, button edge,
   key press/release, wheel, text, paste, or display-size change.  Computed in input_update
   and cleared at the next call.  Read by frame_begin to gate the frontend-dirty check. */
static bool s_io_dirty;

/* Set by gui_owned_window_event (gui_frame.c, same unity TU) when a floater OS window
   is resized.  Consumed and cleared by input_update so the resize marks one frame dirty. */
static bool s_viewport_dirty;

/* Previous primary display size -- compared each frame to detect host-side viewport_resize
   calls on the main surface (which also change win_w/win_h passed into input_update). */
static i32 s_prev_disp_w, s_prev_disp_h;

/* Internal accessors used by gui_frame.c (same unity TU). */
static bool io_dirty( void ) { return s_io_dirty; }

/* Pending text + scroll accumulated between frames by gui_event().  The app event
   ring is single-consumer, and the host must drain it itself (for resize etc.), so
   gui can no longer drain it here -- the host forwards each event to gui_event().
   input_update() moves this pending state into s_io and clears it each frame. */
static char s_pending_text[ sizeof( ( (gui_io_t*)0 )->text ) ];
static u32  s_pending_text_len;
static f32  s_pending_wheel;
static char s_pending_paste[ sizeof( ( (gui_io_t*)0 )->paste ) ];
static bool s_pending_paste_set;   /* a paste arrived this frame (distinguishes "" paste) */

/* The OS window the cursor is currently in, learned from the win_id on mouse move/button/wheel
   events (the polled position alone carries no window identity).  Win32 holds mouse capture on the
   origin window while a button is down, so during a drag these events keep arriving from that
   same window even if the cursor leaves it.  Cleared to invalid on up so it re-learns. */
static i32  s_pending_mouse_win = APP_WIN_INVALID;
static bool s_pending_mouse_win_set;

static bool s_debug_enabled;

void gui_debug_enable( bool enable )
{
    s_debug_enabled = enable;
}

bool gui_debug_is_enabled( void )
{
    return s_debug_enabled;
}

/* Double-click detection.  gui has no clock of its own, so the second press of a pair is
   recognised from the dt fed to frame_begin: a press counts as a double-click when it lands
   within DOUBLE_CLICK_TIME seconds of the previous press and within DOUBLE_CLICK_DIST pixels.
   s_click_elapsed grows by dt each frame and resets on every fresh press. */
#define DOUBLE_CLICK_TIME  0.30f    /* seconds between the two presses */
#define DOUBLE_CLICK_DIST  6.0f     /* max cursor travel between them (pixels) */

static f32 s_click_elapsed[ 3 ] = { 1.0e9f, 1.0e9f, 1.0e9f };   /* start "long ago" */
static f32 s_click_x[ 3 ], s_click_y[ 3 ];

/*----------------------------------------------------------------------------------------------
    Clipboard

    Outbound (cut / copy) goes straight to the OS clipboard through the app module
    (app()->clipboard_set), so copied text is available to other applications.  Inbound (paste)
    is event-driven: the platform reads the OS clipboard on the paste gesture and posts an
    APP_EV_CLIPBOARD event, which gui_event copies into the pending-paste buffer below; the
    next input_update promotes it to s_io.paste for the widget code to consume.  gui owns no
    clipboard buffer of its own -- it is a pure conduit between the OS and the focused field.
----------------------------------------------------------------------------------------------*/

/* Copy n bytes of `s` to the OS clipboard, dropping control characters (a single-line field's
   selection never legitimately contains any, but this keeps the published text clean).  Builds
   a NUL-terminated temporary because the source is a slice of a larger buffer. */
static void
gui_clipboard_set( const char* s, u32 n )
{
    char tmp[ sizeof( ( (gui_io_t*)0 )->paste ) ];
    u32  w = 0;
    for ( u32 i = 0; i < n && w + 1u < sizeof( tmp ); ++i )
        if ( (u8)s[ i ] >= 0x20u && (u8)s[ i ] != 0x7Fu )
            tmp[ w++ ] = s[ i ];
    tmp[ w ] = '\0';
    app()->clipboard_set( tmp );
}

/* Stage pasted text arriving via APP_EV_CLIPBOARD; promoted to s_io.paste by input_update. */
static void
add_paste_text( const char* text )
{
    u32 i = 0;
    if ( text )
        for ( ; text[ i ] && i + 1u < sizeof( s_pending_paste ); ++i )
            s_pending_paste[ i ] = text[ i ];
    s_pending_paste[ i ]  = '\0';
    s_pending_paste_set   = true;
}

/*----------------------------------------------------------------------------------------------
    Internal input feeders -- fed by gui_event() as it unpacks the app event ring,
    before gui_frame_begin() for the same frame.  Not part of the public API.
----------------------------------------------------------------------------------------------*/

static void
add_input_char( u32 codepoint )
{
    /* Ignore control characters.  Windows posts WM_CHAR for backspace (0x08), tab,
       enter, escape, DEL (0x7F) etc.; those are handled via key state, not inserted
       as text -- without this, backspace would append '\b' that its own delete then
       removes, so it appears to do nothing. */
    if ( codepoint < 0x20u || codepoint == 0x7Fu )
        return;

    /* ASCII only: codepoints >127 collapse to '?'. */
    if ( s_pending_text_len + 1u < sizeof( s_pending_text ) )
    {
        s_pending_text[ s_pending_text_len++ ] = ( codepoint < 128u ) ? (char)codepoint : '?';
        s_pending_text[ s_pending_text_len   ] = '\0';
    }
}

static void
add_mouse_wheel( f32 delta )
{
    s_pending_wheel += delta;
}

/* Forward one drained app event to gui.  The host loops its event ring and
   passes every event here; gui unpacks the input events it cares about (text +
   scroll) so that logic lives in one place instead of in every host's switch.
   Returns true when gui consumed the event, letting hosts skip their own
   handling for it (e.g. `if ( gui()->event( &ev ) ) continue;`). */

bool
gui_event( const app_event_t* ev )
{
    switch ( ev->type )
    {
        case APP_EV_CHAR:
            add_input_char( ev->data.text.codepoint );
            return true;

        case APP_EV_MOUSE_WHEEL:
            s_pending_mouse_win     = ev->win_id;   /* route the wheel to the cursor's surface */
            s_pending_mouse_win_set = true;
            add_mouse_wheel( (f32)ev->data.mouse_wheel.delta );
            return true;

        case APP_EV_CLIPBOARD:
            add_paste_text( ev->data.clipboard.text );
            return true;

        case APP_EV_KEY_DOWN:
        {
            if ( s_debug_enabled )
            {
                switch ( ev->data.key.key )
                {
                    case APP_KEY_F1: gui_debug_set_layers( gui_debug_get_layers() ^ GUI_DBG_WINDOW );   return true;
                    case APP_KEY_F2: gui_debug_set_layers( gui_debug_get_layers() ^ GUI_DBG_INTERACT ); return true;
                    case APP_KEY_F3: gui_debug_set_layers( gui_debug_get_layers() ^ GUI_DBG_RESIZE );   return true;
                    case APP_KEY_F4: gui_debug_set_layers( gui_debug_get_layers() ^ GUI_DBG_CLIP );     return true;
                    case APP_KEY_F5: gui_debug_set_layers( gui_debug_get_layers() ^ GUI_DBG_LAYOUT );   return true;
                    default: break;
                }
            }
            return false;
        }

        /* Position + buttons are still resolved by input_update from the polled snapshot (client
           coords of the window the cursor is in); these events carry the win_id that identifies
           WHICH window that is, so the host viewport can be resolved.  Not consumed -- the
           mouse-capture fence (want_capture_mouse) decides UI-vs-scene at read time, not here. */
        case APP_EV_MOUSE_MOVE:
        case APP_EV_MOUSE_DOWN:
        case APP_EV_MOUSE_UP:
            s_pending_mouse_win     = ev->win_id;
            s_pending_mouse_win_set = true;
            return false;

        /* An gui-OWNED floater's OS window resize/close is gui's to service (it owns that
           window + rhi context).  Delegate to the viewport-pool helper: it consumes the event
           (returns true) only when win_id is an owned surface, so a host window's resize/close
           still falls through to the host. */
        case APP_EV_WIN_RESIZE:
        case APP_EV_WIN_CLOSE:
            return gui_owned_window_event( ev );

        default:
            return false;
    }
}

/*----------------------------------------------------------------------------------------------
    input_update -- populate s_io for the current frame.
----------------------------------------------------------------------------------------------*/

static void
input_update( i32 win_w, i32 win_h, f32 dt )
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

    /* Double-click: a press soon after, and close to, the previous press.  Done before the
       text/scroll merge below so it is ready for the widget code this frame. */
    for ( u32 i = 0; i < 3; ++i )
    {
        s_io.mouse_double[ i ] = false;
        s_click_elapsed[ i ]  += dt;

        if ( s_io.mouse_pressed[ i ] )
        {
            f32 dx = s_io.mouse_x - s_click_x[ i ];
            f32 dy = s_io.mouse_y - s_click_y[ i ];
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

    /* Text + scroll + paste arrive via the host-fed pending state (the host owns the event
       ring drain).  Move it into the frame snapshot, then clear it for next frame.  s_io.paste
       is non-empty only on the single frame a paste event was seen, so the focused field
       applies it exactly once. */
    memcpy( s_io.text, s_pending_text, (size_t)s_pending_text_len + 1u );
    s_io.mouse_wheel = s_pending_wheel;

    if ( s_pending_paste_set )
        memcpy( s_io.paste, s_pending_paste, sizeof( s_io.paste ) );
    else
        s_io.paste[ 0 ] = '\0';

    /* Display size change: primary window resized (win_w/win_h changed) or a floater viewport
       was resized (s_viewport_dirty set by gui_owned_window_event).  Either invalidates the
       cached layout -- window clip rects and draw_reset dimensions must be recomputed. */
    bool disp_changed = ( win_w != s_prev_disp_w || win_h != s_prev_disp_h ) || s_viewport_dirty;
    s_prev_disp_w   = win_w;
    s_prev_disp_h   = win_h;
    s_viewport_dirty = false;

    /* Frame is dirty when anything changed vs last frame: position, button/key edges (including
       repeat ticks), wheel, typed text, clipboard paste, or display-size change. */
    s_io_dirty = mouse_moved || mouse_edge || key_edge || disp_changed
              || ( s_pending_wheel    != 0.0f )
              || ( s_pending_text_len >  0    )
              || s_pending_paste_set;

    s_pending_text[ 0 ]  = '\0';
    s_pending_text_len   = 0;
    s_pending_wheel      = 0.0f;
    s_pending_paste[ 0 ] = '\0';
    s_pending_paste_set  = false;

    s_io.display_w = win_w;
    s_io.display_h = win_h;
    s_io.dt        = dt;
    s_io.time     += (f64)dt;   /* monotonic frame clock for get_time() */
}

/* Modifier key helpers: poll both L and R variants so callers need not repeat the pair. */
static bool io_ctrl ( void ) { return s_io.keys_down[ APP_KEY_LCTRL  ] || s_io.keys_down[ APP_KEY_RCTRL  ]; }
static bool io_shift( void ) { return s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ]; }
static bool io_alt  ( void ) { return s_io.keys_down[ APP_KEY_LALT   ] || s_io.keys_down[ APP_KEY_RALT   ]; }

// clang-format on
/*============================================================================================*/
