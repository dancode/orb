/*==============================================================================================

    engine/app/win/win_window.c — Win32 window backend.

    Window pool
    -----------
    Up to APP_WIN_MAX windows are held in a fixed pool. Each slot is identified
    by a win_id_t (0 … APP_WIN_MAX-1). g_pool.alloc is a bitmask — bit i set
    means slot i is occupied. The first window opened is recorded as main_id;
    closing it sets g_app_quit so pump_events returns false.

    Per-window back-pointer
    -----------------------
    CreateWindowExW stores NULL in GWLP_USERDATA until we call SetWindowLongPtrW
    right after creation. WndProc reads that pointer at the top of every message;
    messages that arrive before or after the pointer is valid fall through to
    DefWindowProcW harmlessly.

    State tracking
    --------------
    Each app_window_t carries state (current) and prev (previous frame). The
    WndProc updates state in-place as WM_SIZE / WM_SETFOCUS / WM_KILLFOCUS
    arrive. pump_events() snapshots prev = state at the top of the frame so
    callers can compute changed = state.bits XOR prev.bits.

    Class lifetime
    --------------
    The WNDCLASSEXW is registered on the first window_open and unregistered
    when the last window is closed, so hosts that open / close / reopen windows
    work correctly.

==============================================================================================*/

/* windows.h is included by app.c before this file. */

/*==============================================================================================
    State
==============================================================================================*/

#define APP_WINDOW_CLASS_W L"orb_app_window"

/*==============================================================================================
    Pool helpers
==============================================================================================*/

static win_id_t
win_alloc_slot( void )
{
    for ( int i = 0; i < APP_WIN_MAX; ++i )
    {
        if ( !( g_pool.alloc & ( 1u << i ) ) )
        {
            g_pool.alloc |= ( 1u << i );
            return ( win_id_t )i;
        }
    }
    return APP_WIN_INVALID;
}

static void
win_free_slot( win_id_t id )
{
    if ( id >= 0 && id < APP_WIN_MAX )
        g_pool.alloc &= ~( 1u << id );
}

static app_window_t*
win_get( win_id_t id )
{
    if ( id < 0 || id >= APP_WIN_MAX )
        return NULL;
    if ( !( g_pool.alloc & ( 1u << id ) ) )
        return NULL;
    return &g_pool.wins[ id ];
}

/*==============================================================================================
    API implementations
==============================================================================================*/

static win_id_t
app_window_open( const char* title, i32 x, i32 y, i32 w, i32 h, u32 flags )
{
    /* Register the window class on first use. */
    if ( !g_pool.hinst )
    {
        g_pool.hinst = GetModuleHandleW( NULL );

        /* style: no CS_HREDRAW/CS_VREDRAW -- RHI owns pixels, suppress resize invalidation */
        /* .hbrBackground: no brush -- renderer is responsible for all pixel content */
        /* .hCursor: standard arrow -- without it DefWindowProc leaves the border resize
           cursor active over the client area on WM_SETCURSOR. Non-client borders still
           get their resize cursors (resolved by hit-test before the class cursor). */

        WNDCLASSEXW wc = {
            .cbSize        = sizeof( wc ),
            .style         = 0,
            .lpfnWndProc   = app_wnd_proc,
            .hInstance     = g_pool.hinst,
            .hCursor       = LoadCursorW( NULL, ( LPCWSTR )IDC_ARROW ),
            .hbrBackground = NULL,
            .lpszClassName = APP_WINDOW_CLASS_W,
        };

        g_pool.class_atom = RegisterClassExW( &wc );
        if ( !g_pool.class_atom )
        {
            app_log( ORB_LOG_ERROR, "RegisterClassExW failed (err %lu)", GetLastError() );
            return APP_WIN_INVALID;
        }
    }

    win_id_t id = win_alloc_slot();
    if ( id == APP_WIN_INVALID )
    {
        app_log( ORB_LOG_ERROR, "window pool full (max %d)", APP_WIN_MAX );
        return APP_WIN_INVALID;
    }

    app_window_t* win = &g_pool.wins[ id ];
    win->id            = id;
    win->state         = ( app_win_state_t ){ 0 };
    win->prev          = win->state;
    win->w             = w;
    win->h             = h;


    /* Map app flags → Win32 style */
    DWORD style    = 0;
    DWORD ex_style = 0;

    if ( flags & APP_WIN_FILLSCREEN )
    {
        style    = WS_POPUP;
        ex_style = WS_EX_TOPMOST;
    }
    else if ( flags & APP_WIN_BORDERLESS )
    {
        /* Borderless yet native-capable.  Use a CAPTION-class (overlapped) style, NOT WS_POPUP:
           the visible caption + borders are stripped in WM_NCCALCSIZE (client fills the whole
           window), but keeping WS_CAPTION makes the OS treat this as a normal application window --
           so it plays the native minimize / maximize / restore animations, gets a taskbar button,
           and supports Aero Snap.  A pure WS_POPUP gets none of those (it pops in/out instantly and
           minimizes to a legacy title-bar stub).  WS_THICKFRAME keeps the sizing loop live for the
           border resize; the min/max box + sysmenu keep those native actions reachable.  The imgui
           titlebar drives all of it through WM_NCHITTEST. */
        style = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    }
    else if ( flags & APP_WIN_POPUP )
    {
        style = WS_POPUP;
    }
    else
    {
        if ( flags & APP_WIN_TITLE  ) style |= WS_CAPTION | WS_SYSMENU;
        if ( flags & APP_WIN_RESIZE ) style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        if ( !style ) style = WS_OVERLAPPEDWINDOW;
    }

    if ( flags & APP_WIN_TOPMOST ) ex_style |= WS_EX_TOPMOST;
    if ( flags & APP_WIN_TOOL   ) ex_style |= WS_EX_TOOLWINDOW;

    win->has_resize = ( style & WS_THICKFRAME ) != 0;

    /* Custom-frame state: a borderless window claims its whole rect as client (WM_NCCALCSIZE) and
       returns HTCLIENT from WM_NCHITTEST for everything inside the edge-resize band.  The resize
       grab defaults to the OS sizing-frame width so the borders are draggable at once. */
    win->native.enabled = ( flags & APP_WIN_BORDERLESS ) != 0;
    win->native.border  = win->native.enabled
                          ? GetSystemMetrics( SM_CXSIZEFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER )
                          : 0;

    /* Determine client size — 0 then 50% of desktop work area */
    if ( w <= 0 || h <= 0 )
    {
        RECT work = { 0 };
        SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
        if ( w <= 0 ) w = ( work.right  - work.left ) / 2;
        if ( h <= 0 ) h = ( work.bottom - work.top  ) / 2;
    }

    /* Frame size.  A native custom-frame window claims its whole rect as client (WM_NCCALCSIZE),
       so window == client: skip AdjustWindowRectEx, or it would inflate the rect by the (about to be
       stripped) caption + borders and the client would end up larger than the requested w x h. */
    RECT rect = { 0, 0, w, h };
    if ( !win->native.enabled )
        AdjustWindowRectEx( &rect, style, FALSE, ex_style );

    /* x = y = 0 then OS positions (CW_USEDEFAULT) */
    i32 win_x = ( x == 0 && y == 0 ) ? CW_USEDEFAULT : x;
    i32 win_y = ( x == 0 && y == 0 ) ? CW_USEDEFAULT : y;

    wchar_t wide_title[ 256 ];
    if ( !title || MultiByteToWideChar( CP_UTF8, 0, title, -1, wide_title, 256 ) == 0 )
        wide_title[ 0 ] = L'\0';

    HWND hwnd = CreateWindowExW( ex_style, APP_WINDOW_CLASS_W, wide_title, style,
                                 win_x, win_y,
                                 rect.right - rect.left, rect.bottom - rect.top,
                                 NULL, NULL, g_pool.hinst, NULL );

    if ( !hwnd )
    {
        app_log( ORB_LOG_ERROR, "CreateWindowExW failed (err %lu)", GetLastError() );
        win_free_slot( id );
        return APP_WIN_INVALID;
    }

    win->hwnd = hwnd;
    SetWindowLongPtrW( hwnd, GWLP_USERDATA, ( LONG_PTR )win );

    /* The WM_NCCALCSIZE during CreateWindowExW ran before the back-pointer above was set, so the
       custom frame was not yet stripped.  Re-fire it now that the WndProc can see win->native. */
    if ( win->native.enabled )
        SetWindowPos( hwnd, NULL, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );

    if ( g_pool.main_id == APP_WIN_INVALID )
    {
        g_pool.main_id = id;
#ifdef APP_WIN_FIBER
        win_fiber_init();
#endif
    }

    /* Show window */
    if ( flags & APP_WIN_HIDDEN )
    {
        ShowWindow( hwnd, SW_HIDE );
        win->state.hidden = 1;
    }
    else if ( flags & APP_WIN_NOFOCUS )
    {
        ShowWindow( hwnd, SW_SHOWNOACTIVATE );
    }
    else if ( flags & APP_WIN_MAXIMIZE )
    {
        ShowWindow( hwnd, SW_SHOWMAXIMIZED );
        win->state.maximized = 1;
    }
    else if ( flags & APP_WIN_MINIMIZE )
    {
        ShowWindow( hwnd, SW_SHOWMINIMIZED );
        win->state.minimized = 1;
    }
    else
    {
        ShowWindow( hwnd, SW_SHOWNORMAL );
        win->state.restored = 1;
    }

    UpdateWindow( hwnd );
    win->prev = win->state;

    app_log( ORB_LOG_INFO, "window %d opened (%dx%d client)", id, w, h );
    return id;
}

static void
app_window_close( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( !win )
        return;

    if ( win->hwnd )
    {
        /* Clear the back-pointer before DestroyWindow so WndProc ignores
           the WM_DESTROY-triggered cleanup messages. */
        SetWindowLongPtrW( win->hwnd, GWLP_USERDATA, 0 );
        DestroyWindow( win->hwnd );
        win->hwnd = NULL;
    }

    if ( id == g_pool.main_id )
        g_pool.main_id = APP_WIN_INVALID;

    win_free_slot( id );

    /* Unregister class when the last window is gone. */
    if ( !g_pool.alloc && g_pool.class_atom )
    {
        UnregisterClassW( APP_WINDOW_CLASS_W, g_pool.hinst );
        g_pool.class_atom = 0;
    }

#ifdef APP_WIN_FIBER
    if ( !g_pool.alloc && g_pool.fiber_main )
        win_fiber_exit();
#endif

    app_log( ORB_LOG_INFO, "window %d closed", id );
}

static bool
app_window_is_valid( win_id_t id )
{
    return win_get( id ) != NULL;
}


static void
app_window_set_cursor( win_id_t id, app_cursor_t cursor )
{
    app_window_t* win = win_get( id );
    if ( !win )
        return;
    
    win->cursor = cursor;
    
    /* If the cursor is inside the client area, force an immediate update */
    if ( win->state.hovered )
    {
        POINT pt;
        GetCursorPos( &pt );
        if ( WindowFromPoint( pt ) == win->hwnd )
            PostMessageW( win->hwnd, WM_SETCURSOR, ( WPARAM )win->hwnd, MAKELPARAM( HTCLIENT, WM_MOUSEMOVE ) );
    }
}

static void*
app_window_handle( win_id_t id )
{
    app_window_t* win = win_get( id );
    return win ? ( void* )win->hwnd : NULL;
}

static bool
app_window_is_minimized( win_id_t id )
{
    app_window_t* win = win_get( id );
    return win ? ( bool )win->state.minimized : false;
}

/* Window CLIENT-area top-left in virtual-desktop screen coordinates.  Client (not frame) origin so
   it pairs exactly with window_set_pos and with the drawable area a renderer fills -- the screen
   position of the pixel a window draws at (0,0).  (0,0) on an invalid / handle-less window. */
static void
app_window_get_pos( win_id_t id, i32* out_x, i32* out_y )
{
    app_window_t* win = win_get( id );
    POINT         p   = { 0, 0 };
    if ( win && win->hwnd )
        ClientToScreen( win->hwnd, &p );   /* map client (0,0) -> screen */
    if ( out_x ) *out_x = ( i32 )p.x;
    if ( out_y ) *out_y = ( i32 )p.y;
}

/* Move a window so its CLIENT-area top-left lands at screen coordinates (x,y) -- the inverse of
   window_get_pos.  The frame origin is derived from the current client offset (title bar + borders),
   so a bordered window's CLIENT corner lands exactly on (x,y); a borderless window has a zero offset
   and the frame moves there directly.  Size and z-order are left untouched.  No-op on an invalid id.
   This is the move primitive a multi-window UI uses to keep a torn-off OS window tracking its panel. */
static void
app_window_set_pos( win_id_t id, i32 x, i32 y )
{
    app_window_t* win = win_get( id );
    if ( !win || !win->hwnd )
        return;

    /* Frame-to-client offset = client origin - window (frame) origin, both in screen coords. */
    RECT wr = { 0 };
    GetWindowRect( win->hwnd, &wr );
    POINT c = { 0, 0 };
    ClientToScreen( win->hwnd, &c );
    i32 off_x = ( i32 )c.x - ( i32 )wr.left;
    i32 off_y = ( i32 )c.y - ( i32 )wr.top;

    SetWindowPos( win->hwnd, NULL, x - off_x, y - off_y, 0, 0,
                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
}

static app_win_state_t
app_window_state( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( !win )
        return ( app_win_state_t ){ 0 };
    return win->state;
}

static bool
app_pump_events( void )
{
    /* Snapshot input and window state at frame boundary so pressed/released
       and state-changed deltas reflect exactly one frame of transitions. */
    input_snapshot();

    for ( int i = 0; i < APP_WIN_MAX; ++i )
    {
        if ( g_pool.alloc & ( 1u << i ) )
            g_pool.wins[ i ].prev = g_pool.wins[ i ].state;
    }

#ifdef APP_WIN_FIBER
    if ( g_pool.fiber_message )
    {
        /* Yield to message fiber; returns here when fiber drains the queue or
           the 1 ms timer fires during a DefWindowProc modal loop. */
        SwitchToFiber( g_pool.fiber_message );
        return !g_app_quit;
    }
#endif

    /* Fallback: direct PeekMessage poll (no fiber or fiber not yet initialised). */
    MSG msg;
    while ( PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ) )
    {
        if ( msg.message == WM_QUIT )
            g_app_quit = true;

        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    return !g_app_quit;
}

/*============================================================================================*/
