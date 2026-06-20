/*==============================================================================================

    runtime_service/imgui/imgui_input.c -- App input -> imgui IO snapshot.

    input_update() is called once per frame before any widget calls.  It:
        1. Samples mouse position and button state via the app() query API.
        2. Samples per-key down/pressed state for all APP_KEY_COUNT keys.
        3. Consumes text + scroll fed in since the last frame by imgui_event().
    The app event ring is single-consumer; the host drains it (for resize etc.) and
    hands each event to imgui_event(), which unpacks CHAR / MOUSE_WHEEL internally,
    so imgui does not drain the ring itself.  The result is stored in s_io and read
    by the widget code.

    Included by imgui.c after imgui_draw.c.

==============================================================================================*/
#include "runtime_service/imgui/imgui_internal.h"   /* imgui_io_t, IMGUI_KEY_COUNT */
// clang-format off

/* The IO snapshot type (imgui_io_t) and IMGUI_KEY_COUNT are defined in imgui_internal.h. */

/* Compile-time check: IMGUI_KEY_COUNT must be large enough to index all app keys. */
ORB_STATIC_ASSERT( APP_KEY_COUNT <= IMGUI_KEY_COUNT,
                   "IMGUI_KEY_COUNT too small for APP_KEY_COUNT" );

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static imgui_io_t s_io;

/* Pending text + scroll accumulated between frames by imgui_event().  The app event
   ring is single-consumer, and the host must drain it itself (for resize etc.), so
   imgui can no longer drain it here -- the host forwards each event to imgui_event().
   input_update() moves this pending state into s_io and clears it each frame. */
static char s_pending_text[ sizeof( ( (imgui_io_t*)0 )->text ) ];
static u32  s_pending_text_len;
static f32  s_pending_wheel;
static char s_pending_paste[ sizeof( ( (imgui_io_t*)0 )->paste ) ];
static bool s_pending_paste_set;   /* a paste arrived this frame (distinguishes "" paste) */

/* The OS window the cursor is currently in, learned from the win_id on mouse move/button/wheel
   events (the polled position alone carries no window identity).  Win32 holds mouse capture on the
   origin window while a button is down, so during a drag these events keep arriving from that
   window -- the cursor's surface stays bound to where the drag began with no extra latching.  Only
   updated when a mouse event arrives; otherwise the last value stands (the cursor has not moved
   to another window). */
static i32  s_pending_mouse_win;       /* win_id of the most recent mouse event this frame */
static bool s_pending_mouse_win_set;   /* a mouse event arrived this frame -> resolve the viewport */

/* Double-click detection.  imgui has no clock of its own, so the second press of a pair is
   recognised from the dt fed to new_frame: a press counts as a double-click when it lands
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
    APP_EV_CLIPBOARD event, which imgui_event copies into the pending-paste buffer below; the
    next input_update promotes it to s_io.paste for the widget code to consume.  imgui owns no
    clipboard buffer of its own -- it is a pure conduit between the OS and the focused field.
----------------------------------------------------------------------------------------------*/

/* Copy n bytes of `s` to the OS clipboard, dropping control characters (a single-line field's
   selection never legitimately contains any, but this keeps the published text clean).  Builds
   a NUL-terminated temporary because the source is a slice of a larger buffer. */
static void
imgui_clipboard_set( const char* s, u32 n )
{
    char tmp[ sizeof( ( (imgui_io_t*)0 )->paste ) ];
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
    Internal input feeders -- fed by imgui_event() as it unpacks the app event ring,
    before imgui_new_frame() for the same frame.  Not part of the public API.
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

/* Forward one drained app event to imgui.  The host loops its event ring and
   passes every event here; imgui unpacks the input events it cares about (text +
   scroll) so that logic lives in one place instead of in every host's switch.
   Returns true when imgui consumed the event, letting hosts skip their own
   handling for it (e.g. `if ( imgui()->event( &ev ) ) continue;`). */

bool
imgui_event( const app_event_t* ev )
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

        /* An imgui-OWNED floater's OS window resize/close is imgui's to service (it owns that
           window + rhi context).  Delegate to the viewport-pool helper: it consumes the event
           (returns true) only when win_id is an owned surface, so a host window's resize/close
           still falls through to the host. */
        case APP_EV_WIN_RESIZE:
        case APP_EV_WIN_CLOSE:
            return imgui_owned_window_event( ev );

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
    /* Mouse position (polled): client coords of the window the cursor is in. */
    {
        i32 mx = 0, my = 0;
        app()->mouse_position( &mx, &my );
        s_io.mouse_x = (f32)mx;
        s_io.mouse_y = (f32)my;
    }

    /* Resolve the surface the cursor is in from the most recent mouse event's win_id.  Only when a
       mouse event actually arrived this frame -- otherwise the cursor has not crossed to another
       window, so the last resolved viewport still holds (s_io persists across frames). */
    if ( s_pending_mouse_win_set )
    {
        s_io.mouse_viewport     = viewport_index_for_window( s_pending_mouse_win );
        s_pending_mouse_win_set = false;
    }

    /* Mouse button snapshot (left=0, right=1, middle=2). */
    {
        const app_mouse_button_t map[ 3 ] = {
            APP_MOUSE_LEFT, APP_MOUSE_RIGHT, APP_MOUSE_MIDDLE
        };
        for ( u32 i = 0; i < 3; ++i )
        {
            s_io.mouse_down     [ i ] = app()->mouse_button_down     ( map[ i ] );
            s_io.mouse_pressed  [ i ] = app()->mouse_button_pressed  ( map[ i ] );
            s_io.mouse_released [ i ] = app()->mouse_button_released ( map[ i ] );
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
       auto-repeat ticks (held backspace / arrows in a text field), the caller picking which it reads. */
    for ( i32 k = 0; k < APP_KEY_COUNT; ++k )
    {
        s_io.keys_down           [ k ] = app()->key_down           ( (app_key_t)k );
        s_io.keys_pressed        [ k ] = app()->key_pressed        ( (app_key_t)k );
        s_io.keys_pressed_repeat [ k ] = app()->key_pressed_repeat ( (app_key_t)k );
        s_io.keys_released       [ k ] = app()->key_released       ( (app_key_t)k );
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

// clang-format on
/*============================================================================================*/
