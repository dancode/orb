/*==============================================================================================

    engine/app/win/win_input.c — Win32 input backend.

    State model
    -----------
    Two parallel bool arrays per device (keys, mouse buttons): `current` and
    `previous`. WndProc handlers update `current` as messages arrive.
    pump_events() calls input_snapshot() at the top of each frame, which copies
    `current` into `previous`. Query results:

        key_down(k)     = current[k]
        key_pressed(k)  = current[k] && !previous[k]
        key_released(k) = !current[k] && previous[k]

    Focus loss
    ----------
    When the window loses focus we clear `current` (input_clear_all_state).
    This avoids "stuck keys" — without it, releasing a key while the window
    is unfocused would leave it reading as held down after refocus.

==============================================================================================*/
/*==============================================================================================
    State
==============================================================================================*/

typedef struct app_input_s
{
    bool current_keys[ APP_KEY_COUNT ];
    bool previous_keys[ APP_KEY_COUNT ];

    bool current_mouse[ APP_MOUSE_BUTTON_COUNT ];
    bool previous_mouse[ APP_MOUSE_BUTTON_COUNT ];

    i32  mouse_x;
    i32  mouse_y;

} app_input_t;

static app_input_t g_input;

/*==============================================================================================
    Virtual key code → app_key_t mapping
==============================================================================================*/
/* clang-format off */

static app_key_t
vk_to_app_key( WPARAM vk )
{
    /* Letters and digits share their ASCII codes as Win32 VK values. */
    if ( vk >= 'A' && vk <= 'Z' ) return (app_key_t)( APP_KEY_A + ( vk - 'A' ) );
    if ( vk >= '0' && vk <= '9' ) return (app_key_t)( APP_KEY_0 + ( vk - '0' ) );

    if ( vk >= VK_F1 && vk <= VK_F12 ) return (app_key_t)( APP_KEY_F1 + ( vk - VK_F1 ) );

    switch ( vk )
    {
        case VK_ESCAPE:    return APP_KEY_ESCAPE;
        case VK_RETURN:    return APP_KEY_ENTER;
        case VK_SPACE:     return APP_KEY_SPACE;
        case VK_TAB:       return APP_KEY_TAB;
        case VK_BACK:      return APP_KEY_BACKSPACE;

        case VK_LEFT:      return APP_KEY_LEFT;
        case VK_RIGHT:     return APP_KEY_RIGHT;
        case VK_UP:        return APP_KEY_UP;
        case VK_DOWN:      return APP_KEY_DOWN;

        case VK_SHIFT:     return APP_KEY_SHIFT;
        case VK_CONTROL:   return APP_KEY_CONTROL;
        case VK_MENU:      return APP_KEY_ALT;

        default:           return APP_KEY_NONE;
    }
}

/* clang-format on */

/*==============================================================================================
    Internal handlers (called from win_window.c's WndProc)
==============================================================================================*/

static void
input_handle_key_down( WPARAM vk, LPARAM lp )
{
    /* Ignore auto-repeats. Bit 30 of lparam is the previous key state — set
       means the key was already down, i.e. this is a repeat. We want a single
       pressed-transition per physical key press, not one per repeat. */
    if ( lp & ( ( LPARAM )1 << 30 ) )
        return;

    app_key_t k = vk_to_app_key( vk );
    if ( k != APP_KEY_NONE )
        g_input.current_keys[ k ] = true;
}

static void
input_handle_key_up( WPARAM vk )
{
    app_key_t k = vk_to_app_key( vk );
    if ( k != APP_KEY_NONE )
        g_input.current_keys[ k ] = false;
}

static void
input_handle_mouse_button( app_mouse_button_t btn, bool down )
{
    if ( ( int )btn >= 0 && ( int )btn < APP_MOUSE_BUTTON_COUNT )
        g_input.current_mouse[ btn ] = down;
}

static void
input_handle_mouse_move( LPARAM lp )
{
    /* GET_X_LPARAM / GET_Y_LPARAM inlined: low/high word of lparam,
       sign-extended through i16 so negative coords (drag outside window) work. */
    g_input.mouse_x = ( i32 )( i16 )LOWORD( lp );
    g_input.mouse_y = ( i32 )( i16 )HIWORD( lp );
}

static void
input_clear_all_state( void )
{
    /* Called on WM_KILLFOCUS — we won't see release messages while unfocused. */
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.current_keys[ i ] = false;
    for ( int i = 0; i < APP_MOUSE_BUTTON_COUNT; ++i ) g_input.current_mouse[ i ] = false;
}

/*==============================================================================================
    Frame-boundary snapshot (called from pump_events)
==============================================================================================*/

static void
input_snapshot( void )
{
    for ( int i = 0; i < APP_KEY_COUNT; ++i ) g_input.previous_keys[ i ] = g_input.current_keys[ i ];

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
    return g_input.current_keys[ key ] && !g_input.previous_keys[ key ];
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

/*============================================================================================*/