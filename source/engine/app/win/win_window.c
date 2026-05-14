/*==============================================================================================

    engine/app/win/win_window.c — Win32 window backend.

==============================================================================================*/

/* windows.h is already included by app.c before this file. */

/*==============================================================================================
    State
==============================================================================================*/

#define APP_WINDOW_CLASS_W L"orb_app_window"

typedef struct app_window_s
{
    HINSTANCE hinst;
    ATOM      class_atom;
    HWND      hwnd;
    bool      quit_requested;

} app_window_t;

static app_window_t g_window;

/*==============================================================================================
    Window procedure
==============================================================================================*/

/*==============================================================================================
    Window procedure
==============================================================================================*/

static LRESULT CALLBACK
app_wnd_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch ( msg )
    {
        case WM_DESTROY: PostQuitMessage( 0 ); return 0;

        case WM_KILLFOCUS:
            /* Releases that happen while unfocused will never reach us, so
               clear the state proactively to avoid stuck keys. */
            input_clear_all_state();
            return 0;

            /* ---- Keyboard ---- */

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: input_handle_key_down( wp, lp ); return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            input_handle_key_up( wp );
            return 0;

            /* ---- Mouse buttons ---- */

        case WM_LBUTTONDOWN: input_handle_mouse_button( APP_MOUSE_LEFT, true ); return 0;
        case WM_LBUTTONUP: input_handle_mouse_button( APP_MOUSE_LEFT, false ); return 0;
        case WM_RBUTTONDOWN: input_handle_mouse_button( APP_MOUSE_RIGHT, true ); return 0;
        case WM_RBUTTONUP: input_handle_mouse_button( APP_MOUSE_RIGHT, false ); return 0;
        case WM_MBUTTONDOWN: input_handle_mouse_button( APP_MOUSE_MIDDLE, true ); return 0;
        case WM_MBUTTONUP:
            input_handle_mouse_button( APP_MOUSE_MIDDLE, false );
            return 0;

            /* ---- Mouse motion ---- */

        case WM_MOUSEMOVE: input_handle_mouse_move( lp ); return 0;

        default: return DefWindowProcW( hwnd, msg, wp, lp );
    }
}

/*==============================================================================================
    API implementations
==============================================================================================*/

static bool
app_window_create( const char* title, int width, int height )
{
    if ( g_window.hwnd != NULL )
    {
        printf( "[app] window_create: a window already exists\n" );
        return false;
    }

    g_window.hinst = GetModuleHandleW( NULL );

    WNDCLASSEXW wc = {
        .cbSize        = sizeof( wc ),
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = app_wnd_proc,
        .hInstance     = g_window.hinst,
        .hCursor       = NULL,    // LoadCursorW( NULL, IDC_ARROW ),
        .hbrBackground = ( HBRUSH )( COLOR_WINDOW + 1 ),
        .lpszClassName = APP_WINDOW_CLASS_W,
    };

    g_window.class_atom = RegisterClassExW( &wc );
    if ( !g_window.class_atom )
    {
        printf( "[app] RegisterClassExW failed (err %lu)\n", GetLastError() );
        return false;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT  rect  = { 0, 0, width, height };
    AdjustWindowRect( &rect, style, FALSE );

    wchar_t wide_title[ 256 ];
    if ( MultiByteToWideChar( CP_UTF8, 0, title, -1, wide_title, 256 ) == 0 )
    {
        wide_title[ 0 ] = L'A';
        wide_title[ 1 ] = L'p';
        wide_title[ 2 ] = L'p';
        wide_title[ 3 ] = L'\0';
    }

    g_window.hwnd =
        CreateWindowExW( 0, APP_WINDOW_CLASS_W, wide_title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                         rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, g_window.hinst, NULL );

    if ( !g_window.hwnd )
    {
        printf( "[app] CreateWindowExW failed (err %lu)\n", GetLastError() );
        UnregisterClassW( APP_WINDOW_CLASS_W, g_window.hinst );
        g_window.class_atom = 0;
        return false;
    }

    ShowWindow( g_window.hwnd, SW_SHOWNORMAL );
    UpdateWindow( g_window.hwnd );

    g_window.quit_requested = false;
    printf( "[app] window created (%dx%d client)\n", width, height );
    return true;
}

static void
app_window_destroy( void )
{
    if ( g_window.hwnd )
    {
        DestroyWindow( g_window.hwnd );
        g_window.hwnd = NULL;
    }
    if ( g_window.class_atom )
    {
        UnregisterClassW( APP_WINDOW_CLASS_W, g_window.hinst );
        g_window.class_atom = 0;
    }
    printf( "[app] window destroyed\n" );
}

static bool
app_pump_events( void )
{
    /* Snapshot input state FIRST so pressed/released transitions are computed
       between the previous frame's drain and this frame's. */
    input_snapshot();

    MSG msg;
    while ( PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ) )
    {
        if ( msg.message == WM_QUIT )
            g_window.quit_requested = true;

        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    return !g_window.quit_requested;
}

static void*
app_window_handle( void )
{
    return ( void* )g_window.hwnd;
}

/*============================================================================================*/