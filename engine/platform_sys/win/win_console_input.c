/*==============================================================================================

    win_console_intut.c

==============================================================================================*/

static sys_key_t
sys_key_from_win32_virtual_key( WORD vk )
{
    if ( vk >= 'A' && vk <= 'Z' )
        return ( sys_key_t )( PLATFORM_KEY_A + ( vk - 'A' ) );

    if ( vk >= '0' && vk <= '9' )
        return ( sys_key_t )( PLATFORM_KEY_0 + ( vk - '0' ) );

    switch ( vk )
    {
        case VK_ESCAPE: return PLATFORM_KEY_ESCAPE;
        case VK_RETURN: return PLATFORM_KEY_ENTER;
        case VK_SPACE: return PLATFORM_KEY_SPACE;
        case VK_F1: return PLATFORM_KEY_F1;
        case VK_F2: return PLATFORM_KEY_F2;
        case VK_F3: return PLATFORM_KEY_F3;
        case VK_F4: return PLATFORM_KEY_F4;
        case VK_F5: return PLATFORM_KEY_F5;
        case VK_F6: return PLATFORM_KEY_F6;
        case VK_F7: return PLATFORM_KEY_F7;
        case VK_F8: return PLATFORM_KEY_F8;
        case VK_F9: return PLATFORM_KEY_F9;
        case VK_F10: return PLATFORM_KEY_F10;
        case VK_F11: return PLATFORM_KEY_F11;
        case VK_F12: return PLATFORM_KEY_F12;
        default: return PLATFORM_KEY_NONE;
    }
}

/*============================================================================================*/

typedef struct sys_console_input_state_s
{
    HANDLE input_handle;

    DWORD  original_console_mode;
    bool   has_original_console_mode;
    bool   initialized;

    bool   key_down[ PLATFORM_KEY_COUNT ];
    bool   key_pressed[ PLATFORM_KEY_COUNT ];
    bool   key_released[ PLATFORM_KEY_COUNT ];

} sys_console_input_state_t;

static sys_console_input_state_t g_console_input;

/*============================================================================================*/

bool
sys_console_input_init( void )
{
    sys_console_input_state_t* state = &g_console_input;

    ZeroMemory( state, sizeof( *state ) );

    state->input_handle = GetStdHandle( STD_INPUT_HANDLE );
    if ( state->input_handle == INVALID_HANDLE_VALUE || state->input_handle == NULL )
        return false;

    DWORD mode = 0;
    if ( !GetConsoleMode( state->input_handle, &mode ) )
        return false;

    state->original_console_mode     = mode;
    state->has_original_console_mode = true;

    /*
        Disable line input and echo so key presses are available immediately.

        Normally the console waits until Enter is pressed before giving input.
        For a bootstrap loop, we want immediate key events.

        ENABLE_PROCESSED_INPUT is kept so Ctrl+C still behaves normally.
    */
    DWORD new_mode = mode;

    new_mode &= ~ENABLE_LINE_INPUT;
    new_mode &= ~ENABLE_ECHO_INPUT;

    new_mode |= ENABLE_PROCESSED_INPUT;

    if ( !SetConsoleMode( state->input_handle, new_mode ) )
        return false;

    /*
        Remove old buffered console events so startup does not immediately
        consume stale key presses.
    */
    FlushConsoleInputBuffer( state->input_handle );

    state->initialized = true;
    return true;
}

/*============================================================================================*/

void
sys_console_input_shutdown( void )
{
    sys_console_input_state_t* state = &g_console_input;

    if ( state->initialized && state->has_original_console_mode &&
         state->input_handle != INVALID_HANDLE_VALUE && state->input_handle != NULL )
    {
        SetConsoleMode( state->input_handle, state->original_console_mode );
    }

    ZeroMemory( state, sizeof( *state ) );
}

/*============================================================================================*/

void
sys_console_input_poll( void )
{
    sys_console_input_state_t* state = &g_console_input;

    if ( !state->initialized )
        return;

    for ( int i = 0; i < PLATFORM_KEY_COUNT; ++i )
    {
        state->key_pressed[ i ]  = false;
        state->key_released[ i ] = false;
    }

    DWORD event_count = 0;
    if ( !GetNumberOfConsoleInputEvents( state->input_handle, &event_count ) )
        return;

    while ( event_count > 0 )
    {
        INPUT_RECORD record;
        DWORD        records_read = 0;

        if ( !ReadConsoleInputA( state->input_handle, &record, 1, &records_read ) )
            break;

        if ( records_read == 0 )
            break;

        if ( record.EventType == KEY_EVENT )
        {
            KEY_EVENT_RECORD* key_event = &record.Event.KeyEvent;

            sys_key_t    key = sys_key_from_win32_virtual_key( key_event->wVirtualKeyCode );

            if ( key != PLATFORM_KEY_NONE )
            {
                bool was_down = state->key_down[ key ];
                bool is_down  = key_event->bKeyDown ? true : false;

                /*
                    Ignore auto-repeat as extra "pressed" events.

                    Holding R should not repeatedly trigger hot reload unless
                    you explicitly want that behavior.
                */
                if ( is_down && !was_down )
                {
                    state->key_down[ key ]    = true;
                    state->key_pressed[ key ] = true;
                }
                else if ( !is_down && was_down )
                {
                    state->key_down[ key ]     = false;
                    state->key_released[ key ] = true;
                }
            }
        }

        if ( !GetNumberOfConsoleInputEvents( state->input_handle, &event_count ) )
            break;
    }
}

/*============================================================================================*/

bool
sys_key_down( sys_key_t key )
{
    if ( key <= PLATFORM_KEY_NONE || key >= PLATFORM_KEY_COUNT )
        return false;

    return g_console_input.key_down[ key ];
}

bool
sys_key_pressed( sys_key_t key )
{
    if ( key <= PLATFORM_KEY_NONE || key >= PLATFORM_KEY_COUNT )
        return false;

    return g_console_input.key_pressed[ key ];
}

bool
sys_key_released( sys_key_t key )
{
    if ( key <= PLATFORM_KEY_NONE || key >= PLATFORM_KEY_COUNT )
        return false;

    return g_console_input.key_released[ key ];
}


/*============================================================================================*/