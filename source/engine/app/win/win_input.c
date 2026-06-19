/*==============================================================================================

    engine/app/win/win_input.c — Win32 input backend.

    State model
    -----------
    Per device (keys, mouse buttons): `current` and `previous` bool arrays.
    WndProc handlers update `current` as messages arrive.  pump_events() calls
    input_snapshot() at the top of each frame, which copies `current` into
    `previous`.  Mouse query results:

        button_down(b)     = current[b]
        button_pressed(b)  = current[b] && !previous[b]
        button_released(b) = !current[b] && previous[b]

    Keys carry two per-frame flag arrays, cleared each frame in input_snapshot and
    set as WM_KEYDOWNs arrive: `pressed_keys` on an initial press, `repeat_keys` on
    an OS auto-repeat tick.  key_pressed reads `pressed_keys` (a flag, not a
    current/previous edge -- an edge cannot see a repeat, which arrives while current
    is already high); key_pressed_repeat ORs in `repeat_keys` so a held key re-fires
    at the OS rate.  This is per-query: the caller chooses repeat or not, so there is
    no global mode -- game code reads key_pressed (one press per key), text / nav code
    reads key_pressed_repeat.  down/released stay edge-derived:

        key_down(k)           = current[k]
        key_pressed(k)        = pressed_keys[k]                  (initial press only)
        key_pressed_repeat(k) = pressed_keys[k] || repeat_keys[k]  (+ each OS repeat)
        key_released(k)       = !current[k] && previous[k]

    Focus loss
    ----------
    When the window loses focus we clear `current` (input_clear_all_state).
    This avoids "stuck keys" — without it, releasing a key while the window
    is unfocused would leave it reading as held down after refocus.

    Event ring buffer
    -----------------
    win_post_event() writes 32-byte app_event_t entries into a power-of-2
    ring. head is the next write slot; tail is the next read slot. The ring
    is full when (head + 1) & mask == tail — incoming events are dropped rather
    than overwriting unread ones. Callers drain via app_next_event() each frame.

==============================================================================================*/

_Static_assert( sizeof( app_event_t ) == 32, "app_event_t must be 32 bytes" );

/*==============================================================================================
    Input state
==============================================================================================*/

typedef struct app_input_s
{
    bool current_keys[ APP_KEY_COUNT ];
    bool previous_keys[ APP_KEY_COUNT ];
    bool pressed_keys[ APP_KEY_COUNT ];   /* an initial (non-repeat) key-down arrived this frame */
    bool repeat_keys[ APP_KEY_COUNT ];    /* an OS auto-repeat key-down arrived this frame        */

    bool current_mouse[ APP_MOUSE_BUTTON_COUNT ];
    bool previous_mouse[ APP_MOUSE_BUTTON_COUNT ];

    i32  mouse_x;
    i32  mouse_y;

} app_input_t;

static app_input_t g_input;

/*==============================================================================================
    Event ring buffer
==============================================================================================*/

typedef struct app_event_ring_s
{
    app_event_t buf[ APP_EVENT_MAX ];
    u32         head;    /* next write slot  */
    u32         tail;    /* next read slot   */
    i32         next_id; /* monotonic counter */

} app_event_ring_t;

static app_event_ring_t g_events;
static bool             g_app_quit;

/*==============================================================================================
    Helpers visible to win_window.c (included after this file in the unity build)
==============================================================================================*/

static app_mod_t
win_get_mod( void )
{
    app_mod_t m = { 0 };
    if ( GetKeyState( VK_CONTROL ) & 0x8000 )
        m.ctrl = 1;
    if ( GetKeyState( VK_SHIFT ) & 0x8000 )
        m.shift = 1;
    if ( GetKeyState( VK_MENU ) & 0x8000 )
        m.alt = 1;
    if ( ( GetKeyState( VK_LWIN ) & 0x8000 ) || ( GetKeyState( VK_RWIN ) & 0x8000 ) )
        m.super = 1;
    return m;
}

static app_event_t
win_make_event( i32 type, i32 win_id )
{
    app_event_t ev = { 0 };
    ev.event_id    = g_events.next_id++;
    ev.win_id      = win_id;
    ev.type        = type;
    ev.timestamp   = ( i64 )GetTickCount64();
    return ev;
}

static void
win_post_event( const app_event_t* ev )
{
    u32 next = ( g_events.head + 1 ) & APP_EVENT_MASK;
    if ( next == g_events.tail )
        return; /* ring full — drop incoming event */
    g_events.buf[ g_events.head ] = *ev;
    g_events.head                 = next;
}

/*==============================================================================================
    Clipboard (Win32, CF_TEXT)

    The engine is ASCII-only, so CF_TEXT (ANSI) is the natural exchange format.  win_clipboard_set
    publishes a string to the OS clipboard; win_clipboard_read pulls the current text into a static
    staging buffer whose lifetime spans only until the next read — long enough for the host to copy
    it out of the APP_EV_CLIPBOARD event the same frame it is posted (see input_handle_paste).
==============================================================================================*/

#define WIN_CLIPBOARD_MAX 1024   /* staging cap for a single paste (single-line fields are tiny) */

static char s_clipboard_staging[ WIN_CLIPBOARD_MAX ];

/* Publish NUL-terminated `text` to the OS clipboard as CF_TEXT.  Silently no-ops if the
   clipboard cannot be opened (another process owns it) -- a copy is best-effort, never fatal. */
static void
win_clipboard_set( const char* text )
{
    if ( !text ) return;
    if ( !OpenClipboard( NULL ) ) return;

    EmptyClipboard();

    size_t n = 0;
    while ( text[ n ] ) ++n;                            /* length, sans the NUL */

    HGLOBAL mem = GlobalAlloc( GMEM_MOVEABLE, n + 1u ); /* +1 for the NUL terminator */
    if ( mem )
    {
        char* dst = ( char* )GlobalLock( mem );
        if ( dst )
        {
            for ( size_t i = 0; i < n; ++i ) dst[ i ] = text[ i ];
            dst[ n ] = '\0';
            GlobalUnlock( mem );
            SetClipboardData( CF_TEXT, mem );           /* clipboard now owns `mem` */
        }
        else
        {
            GlobalFree( mem );
        }
    }

    CloseClipboard();
}

/* Read the OS clipboard text into the staging buffer and return it (always NUL-terminated;
   empty string when the clipboard holds no text or cannot be opened).  Control characters are
   left intact here -- the single-line consumer strips them, matching the CHAR-event contract. */
static const char*
win_clipboard_read( void )
{
    s_clipboard_staging[ 0 ] = '\0';

    if ( !IsClipboardFormatAvailable( CF_TEXT ) ) return s_clipboard_staging;
    if ( !OpenClipboard( NULL ) )                 return s_clipboard_staging;

    HGLOBAL mem = GetClipboardData( CF_TEXT );
    if ( mem )
    {
        const char* src = ( const char* )GlobalLock( mem );
        if ( src )
        {
            u32 i = 0;
            while ( src[ i ] && i + 1u < sizeof( s_clipboard_staging ) )
            {
                s_clipboard_staging[ i ] = src[ i ];
                ++i;
            }
            s_clipboard_staging[ i ] = '\0';
            GlobalUnlock( mem );
        }
    }

    CloseClipboard();
    return s_clipboard_staging;
}

/*==============================================================================================
    Virtual key code → app_key_t mapping
==============================================================================================*/
/* clang-format off */

static app_key_t
vk_to_app_key( WPARAM vk, LPARAM lp )
{
    if ( vk >= 'A' && vk <= 'Z' ) return (app_key_t)( APP_KEY_A + ( vk - 'A' ) );
    if ( vk >= '0' && vk <= '9' ) return (app_key_t)( APP_KEY_0 + ( vk - '0' ) );

    if ( vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 ) return (app_key_t)( APP_KEY_NP_0 + ( vk - VK_NUMPAD0 ) );
    if ( vk >= VK_F1      && vk <= VK_F12      ) return (app_key_t)( APP_KEY_F1  + ( vk - VK_F1      ) );

    /* Bit 24 of lParam: extended-key flag — distinguishes numpad Enter from main Enter,
       right Ctrl from left Ctrl, right Alt from left Alt. */
    bool ext = ( lp & ( 1 << 24 ) ) != 0;

    switch ( vk )
    {
        case VK_ESCAPE:    return APP_KEY_ESCAPE;
        case VK_RETURN:    return ext ? APP_KEY_NP_ENTER : APP_KEY_ENTER;
        case VK_SPACE:     return APP_KEY_SPACE;
        case VK_TAB:       return APP_KEY_TAB;
        case VK_BACK:      return APP_KEY_BACKSPACE;

        case VK_LEFT:      return APP_KEY_LEFT;
        case VK_RIGHT:     return APP_KEY_RIGHT;
        case VK_UP:        return APP_KEY_UP;
        case VK_DOWN:      return APP_KEY_DOWN;

        case VK_INSERT:    return APP_KEY_INSERT;
        case VK_DELETE:    return APP_KEY_DELETE;
        case VK_HOME:      return APP_KEY_HOME;
        case VK_END:       return APP_KEY_END;
        case VK_PRIOR:     return APP_KEY_PAGE_UP;
        case VK_NEXT:      return APP_KEY_PAGE_DOWN;

        /* Modifiers — use scancode to split left/right shift; extended flag for ctrl/alt. */
        case VK_SHIFT:
        {
            static UINT vsc_lshift = 0;
            if ( !vsc_lshift ) vsc_lshift = MapVirtualKeyW( VK_LSHIFT, MAPVK_VK_TO_VSC );
            UINT sc = ( lp >> 16 ) & 0xFF;
            return ( sc == vsc_lshift ) ? APP_KEY_LSHIFT : APP_KEY_RSHIFT;
        }
        case VK_CONTROL:   return ext ? APP_KEY_RCTRL  : APP_KEY_LCTRL;
        case VK_MENU:      return ext ? APP_KEY_RALT   : APP_KEY_LALT;
        case VK_LWIN:      return APP_KEY_LSUPER;
        case VK_RWIN:      return APP_KEY_RSUPER;

        case VK_CAPITAL:   return APP_KEY_CAPS_LOCK;
        case VK_NUMLOCK:   return APP_KEY_NUM_LOCK;
        case VK_SCROLL:    return APP_KEY_SCROLL_LOCK;

        case VK_DECIMAL:   return APP_KEY_NP_DOT;
        case VK_ADD:       return APP_KEY_NP_ADD;
        case VK_SUBTRACT:  return APP_KEY_NP_SUB;
        case VK_MULTIPLY:  return APP_KEY_NP_MUL;
        case VK_DIVIDE:    return APP_KEY_NP_DIV;

        case VK_OEM_3:     return APP_KEY_GRAVE;
        case VK_OEM_MINUS: return APP_KEY_MINUS;
        case VK_OEM_PLUS:  return APP_KEY_EQUAL;
        case VK_OEM_4:     return APP_KEY_LBRACKET;
        case VK_OEM_6:     return APP_KEY_RBRACKET;
        case VK_OEM_5:     return APP_KEY_BACKSLASH;
        case VK_OEM_1:     return APP_KEY_SEMICOLON;
        case VK_OEM_7:     return APP_KEY_APOSTROPHE;
        case VK_OEM_COMMA: return APP_KEY_COMMA;
        case VK_OEM_PERIOD:return APP_KEY_PERIOD;
        case VK_OEM_2:     return APP_KEY_SLASH;

        case VK_PAUSE:     return APP_KEY_PAUSE;
        case VK_SNAPSHOT:  return APP_KEY_PRINT_SCREEN;
        case VK_APPS:      return APP_KEY_MENU;

        default:           return APP_KEY_NONE;
    }
}

/* clang-format on */

/*==============================================================================================
    Internal handlers (called from win_window.c's WndProc)
==============================================================================================*/

static void
input_handle_key_down( WPARAM vk, LPARAM lp, bool repeat, win_id_t win_id )
{
    app_key_t k = vk_to_app_key( vk, lp );
    if ( k == APP_KEY_NONE )
        return;

    g_input.current_keys[ k ] = true;

    /* Route the press to the flag the matching query reads: an initial press feeds key_pressed,
       a repeat feeds only key_pressed_repeat.  The caller decides which it wants -- no global mode. */
    if ( repeat ) g_input.repeat_keys[ k ]  = true;
    else          g_input.pressed_keys[ k ] = true;

    app_event_t ev            = win_make_event( APP_EV_KEY_DOWN, win_id );
    ev.data.key.key           = ( i32 )k;
    ev.data.key.press         = 255;
    ev.data.key.repeat        = repeat ? 1u : 0u;
    ev.data.key.mod           = win_get_mod();
    win_post_event( &ev );
}

static void
input_handle_key_up( WPARAM vk, LPARAM lp, win_id_t win_id )
{
    app_key_t k = vk_to_app_key( vk, lp );
    if ( k == APP_KEY_NONE )
        return;

    g_input.current_keys[ k ] = false;

    app_event_t ev            = win_make_event( APP_EV_KEY_UP, win_id );
    ev.data.key.key           = ( i32 )k;
    ev.data.key.press         = 0;
    ev.data.key.mod           = win_get_mod();
    win_post_event( &ev );
}

static void
input_handle_char( WPARAM wp, win_id_t win_id )
{
    app_event_t ev         = win_make_event( APP_EV_CHAR, win_id );
    ev.data.text.codepoint = ( u32 )wp;
    ev.data.text.mod       = win_get_mod();
    win_post_event( &ev );
}

/* Paste gesture: snapshot the OS clipboard into the staging buffer and post an
   APP_EV_CLIPBOARD event pointing at it.  The host drains the ring the same frame and copies
   the text out (imgui_event), so the staging pointer's short lifetime is sufficient. */
static void
input_handle_paste( win_id_t win_id )
{
    app_event_t ev          = win_make_event( APP_EV_CLIPBOARD, win_id );
    ev.data.clipboard.text  = win_clipboard_read();
    win_post_event( &ev );
}

static void
input_handle_mouse_button( app_mouse_button_t btn, bool down, i16 x, i16 y, win_id_t win_id )
{
    if ( ( int )btn < 0 || ( int )btn >= APP_MOUSE_BUTTON_COUNT )
        return;

    g_input.current_mouse[ btn ] = down;

    app_event_t ev               = win_make_event( down ? APP_EV_MOUSE_DOWN : APP_EV_MOUSE_UP, win_id );
    ev.data.mouse_btn.button     = ( i32 )btn;
    ev.data.mouse_btn.x          = x;
    ev.data.mouse_btn.y          = y;
    win_post_event( &ev );
}

static void
input_handle_mouse_move( i16 x, i16 y, win_id_t win_id )
{
    i16 dx                = ( i16 )( x - ( i16 )g_input.mouse_x );
    i16 dy                = ( i16 )( y - ( i16 )g_input.mouse_y );

    g_input.mouse_x       = ( i32 )x;
    g_input.mouse_y       = ( i32 )y;

    app_event_t ev        = win_make_event( APP_EV_MOUSE_MOVE, win_id );
    ev.data.mouse_move.x  = x;
    ev.data.mouse_move.y  = y;
    ev.data.mouse_move.dx = dx;
    ev.data.mouse_move.dy = dy;
    win_post_event( &ev );
}

static void
input_handle_mouse_wheel( WPARAM wp, i16 x, i16 y, win_id_t win_id )
{
    i32         delta         = GET_WHEEL_DELTA_WPARAM( wp ) / WHEEL_DELTA;

    app_event_t ev            = win_make_event( APP_EV_MOUSE_WHEEL, win_id );
    ev.data.mouse_wheel.delta = delta;
    ev.data.mouse_wheel.x     = x;
    ev.data.mouse_wheel.y     = y;
    win_post_event( &ev );
}

static void
input_clear_all_state( void )
{
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.current_keys[ i ] = false;
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.pressed_keys[ i ] = false;
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.repeat_keys[ i ]  = false;
    for ( int i = 0; i < APP_MOUSE_BUTTON_COUNT; ++i ) g_input.current_mouse[ i ] = false;
}

/*==============================================================================================
    Frame-boundary snapshot (called from pump_events)
==============================================================================================*/

static void
input_snapshot( void )
{
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.previous_keys[ i ] = g_input.current_keys[ i ];

    /* Clear the per-frame press flags; each WM_KEYDOWN (initial or repeat) re-sets the
       matching array during this frame's message drain, which follows this snapshot. */
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.pressed_keys[ i ] = false;
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.repeat_keys[ i ]  = false;

    for ( int i = 0; i < APP_MOUSE_BUTTON_COUNT; ++i )
        g_input.previous_mouse[ i ] = g_input.current_mouse[ i ];
}

/*==============================================================================================
    API implementations
==============================================================================================*/

static bool
app_key_down( app_key_t key )
{
    if ( key <= APP_KEY_NONE || key >= APP_KEY_COUNT )
        return false;
    return g_input.current_keys[ key ];
}

static bool
app_key_pressed( app_key_t key )
{
    if ( key <= APP_KEY_NONE || key >= APP_KEY_COUNT )
        return false;
    return g_input.pressed_keys[ key ];   /* initial press only -- OS repeats excluded */
}

static bool
app_key_pressed_repeat( app_key_t key )
{
    if ( key <= APP_KEY_NONE || key >= APP_KEY_COUNT )
        return false;
    return g_input.pressed_keys[ key ] || g_input.repeat_keys[ key ];   /* initial + OS repeats */
}

static bool
app_key_released( app_key_t key )
{
    if ( key <= APP_KEY_NONE || key >= APP_KEY_COUNT )
        return false;
    return !g_input.current_keys[ key ] && g_input.previous_keys[ key ];
}

static void
app_mouse_position( i32* out_x, i32* out_y )
{
    if ( out_x )
        *out_x = g_input.mouse_x;
    if ( out_y )
        *out_y = g_input.mouse_y;
}

/* Absolute cursor position in virtual-desktop screen coordinates (origin = primary monitor
   top-left, spanning all monitors).  Unlike app_mouse_position -- which returns coords in the
   hovered window's client space -- this is window-independent, so it stays correct across a
   captured drag that leaves the origin window: the reference frame a multi-window UI needs to
   place a torn-off OS window where the cursor actually is. */
static void
app_mouse_position_screen( i32* out_x, i32* out_y )
{
    POINT p = { 0, 0 };
    GetCursorPos( &p );
    if ( out_x ) *out_x = ( i32 )p.x;
    if ( out_y ) *out_y = ( i32 )p.y;
}

static bool
app_mouse_button_down( app_mouse_button_t btn )
{
    if ( ( int )btn < 0 || ( int )btn >= APP_MOUSE_BUTTON_COUNT )
        return false;
    return g_input.current_mouse[ btn ];
}

static bool
app_mouse_button_pressed( app_mouse_button_t btn )
{
    if ( ( int )btn < 0 || ( int )btn >= APP_MOUSE_BUTTON_COUNT )
        return false;
    return g_input.current_mouse[ btn ] && !g_input.previous_mouse[ btn ];
}

static bool
app_mouse_button_released( app_mouse_button_t btn )
{
    if ( ( int )btn < 0 || ( int )btn >= APP_MOUSE_BUTTON_COUNT )
        return false;
    return !g_input.current_mouse[ btn ] && g_input.previous_mouse[ btn ];
}

static void
app_clipboard_set( const char* text )
{
    win_clipboard_set( text );
}

static bool
app_next_event( app_event_t* out )
{
    if ( g_events.tail == g_events.head )
        return false;

    *out          = g_events.buf[ g_events.tail ];
    g_events.tail = ( g_events.tail + 1 ) & APP_EVENT_MASK;
    return true;
}

static bool
app_should_quit( void )
{
    return g_app_quit;
}

/*============================================================================================*/
