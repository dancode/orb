/*==============================================================================================

    engine/app/win/win_lifecycle.c — Win32 window lifecycle operations.

    Fillscreen
    ----------
    win_set_fillscreen() removes the window chrome and expands to the current
    monitor's full area. It saves all window state needed for a clean restore.
    The maximized-first edge case matters: Windows does not hide the taskbar when
    going fullscreen from a maximized state, so the window is restored first.

==============================================================================================*/

static void
win_set_fillscreen( app_window_t* win, bool enabled )
{
    win_fillscreen_t* fill = &win->fill;
    HWND              hwnd = win->hwnd;

    if ( fill->is_enabled == enabled )
        return;

    if ( enabled )
    {
        /* Save current state before stripping chrome. If currently maximized,
           restore first — Windows leaves the taskbar visible otherwise. */
        fill->prev_maximized = ( IsZoomed( hwnd ) != 0 );
        if ( fill->prev_maximized )
            SendMessageW( hwnd, WM_SYSCOMMAND, SC_RESTORE, 0 );

        fill->prev_style   = (DWORD)GetWindowLongW( hwnd, GWL_STYLE );
        fill->prev_exstyle = (DWORD)GetWindowLongW( hwnd, GWL_EXSTYLE );
        fill->prev_w       = win->w;
        fill->prev_h       = win->h;

        RECT wr = { 0 };
        GetWindowRect( hwnd, &wr );
        fill->prev_x = wr.left;
        fill->prev_y = wr.top;

        /* Strip decorations. */
        SetWindowLongW( hwnd, GWL_STYLE,
                        fill->prev_style & ~( WS_CAPTION | WS_THICKFRAME ) );
        SetWindowLongW( hwnd, GWL_EXSTYLE,
                        fill->prev_exstyle &
                            ~( WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                               WS_EX_CLIENTEDGE    | WS_EX_STATICEDGE ) );

        /* Expand to the monitor that currently holds most of the window. */
        MONITORINFO mi = { .cbSize = sizeof( mi ) };
        GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &mi );

        SetWindowPos( hwnd, HWND_TOP,
                      mi.rcMonitor.left, mi.rcMonitor.top,
                      mi.rcMonitor.right  - mi.rcMonitor.left,
                      mi.rcMonitor.bottom - mi.rcMonitor.top,
                      SWP_FRAMECHANGED | SWP_NOACTIVATE );

        win->state.fillscreen = 1;
    }
    else
    {
        /* Restore decorations and geometry. */
        SetWindowLongW( hwnd, GWL_STYLE,   fill->prev_style   );
        SetWindowLongW( hwnd, GWL_EXSTYLE, fill->prev_exstyle );

        RECT rect = { 0, 0, fill->prev_w, fill->prev_h };
        AdjustWindowRectEx( &rect, fill->prev_style, FALSE, fill->prev_exstyle );

        SetWindowPos( hwnd, NULL,
                      fill->prev_x, fill->prev_y,
                      rect.right - rect.left, rect.bottom - rect.top,
                      SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER );

        if ( fill->prev_maximized )
            SendMessageW( hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0 );

        win->state.fillscreen = 0;
    }

    fill->is_enabled = enabled;
}

/*----------------------------------------------------------------------------------------------
    API implementations
----------------------------------------------------------------------------------------------*/

static void
app_window_set_fillscreen( win_id_t id, bool enabled )
{
    app_window_t* win = win_get( id );
    if ( win )
        win_set_fillscreen( win, enabled );
}

static void
app_window_toggle_fillscreen( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win )
        win_set_fillscreen( win, !win->fill.is_enabled );
}

/*----------------------------------------------------------------------------------------------
    Programmatic geometry / show-state — resize, minimize, restore.

    All three drive the OS directly (SetWindowPos / ShowWindow), so the resulting WM_SIZE is
    handled by the normal WndProc path: window state flags are updated and an APP_EV_WIN_RESIZE
    is posted.  Callers therefore see the same event flow whether the user dragged the frame or
    the app requested the change.
----------------------------------------------------------------------------------------------*/

static void
app_window_resize( win_id_t id, i32 w, i32 h )
{
    app_window_t* win = win_get( id );
    if ( !win || !win->hwnd || w <= 0 || h <= 0 )
        return;

    /* A minimized or maximized window has no normal client extent to resize into; bring it
       back to the restored state first so the new size actually takes effect (and is what a
       later restore returns to). */
    if ( IsIconic( win->hwnd ) || IsZoomed( win->hwnd ) )
        ShowWindow( win->hwnd, SW_RESTORE );

    /* Convert the requested CLIENT size to the full window rect for the current chrome. */
    DWORD style    = (DWORD)GetWindowLongW( win->hwnd, GWL_STYLE );
    DWORD ex_style = (DWORD)GetWindowLongW( win->hwnd, GWL_EXSTYLE );

    RECT rect = { 0, 0, w, h };
    AdjustWindowRectEx( &rect, style, FALSE, ex_style );

    /* Keep position and z-order; the WM_SIZE this generates updates win->w/h and posts the event. */
    SetWindowPos( win->hwnd, NULL, 0, 0,
                  rect.right - rect.left, rect.bottom - rect.top,
                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
}

static void
app_window_minimize( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        ShowWindow( win->hwnd, SW_MINIMIZE );
}

static void
app_window_restore( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        ShowWindow( win->hwnd, SW_RESTORE );
}

static void
app_window_maximize( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        ShowWindow( win->hwnd, SW_MAXIMIZE );
}

static void
app_window_toggle_maximize( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        ShowWindow( win->hwnd, IsZoomed( win->hwnd ) ? SW_RESTORE : SW_MAXIMIZE );
}

/*----------------------------------------------------------------------------------------------
    Native-borderless window actions.

    A borderless window has no Win32 non-client area, so the imgui titlebar / borders stand in
    for it.  Each grab is handed back to the OS via WM_NCLBUTTONDOWN (move / resize), which runs
    its own blocking modal loop inside DefWindowProc.  That loop must execute on the MESSAGE fiber
    so the timer can keep yielding to the game loop; the game-loop fiber (where imgui calls these)
    cannot run it.  So the primitives only POST a private message -- non-blocking and fiber-safe --
    and the WndProc (win_window_proc.c, on the message fiber) performs the actual modal call.

    win_zone_to_win32 lives here because translating the imgui zone to a Win32 hit-test code is the
    one piece that belongs to the caller side; the resulting code rides in the posted wParam.
----------------------------------------------------------------------------------------------*/

/* app_win_zone_t -> Win32 hit-test code.  Indexed directly by zone; order must match the enum. */
static const LRESULT win_zone_to_win32[ APP_ZONE_COUNT ] = {
    HTNOWHERE,     /* APP_ZONE_NONE        */
    HTTOPLEFT,     /* APP_ZONE_TOPLEFT     */
    HTTOP,         /* APP_ZONE_TOP         */
    HTTOPRIGHT,    /* APP_ZONE_TOPRIGHT    */
    HTLEFT,        /* APP_ZONE_LEFT        */
    HTCLIENT,      /* APP_ZONE_CLIENT      */
    HTRIGHT,       /* APP_ZONE_RIGHT       */
    HTBOTTOMLEFT,  /* APP_ZONE_BOTTOMLEFT  */
    HTBOTTOM,      /* APP_ZONE_BOTTOM      */
    HTBOTTOMRIGHT, /* APP_ZONE_BOTTOMRIGHT */
    HTCAPTION,     /* APP_ZONE_CAPTION     */
    HTMINBUTTON,   /* APP_ZONE_MINBUTTON   */
    HTMAXBUTTON,   /* APP_ZONE_MAXBUTTON   */
    HTCLOSE,       /* APP_ZONE_CLOSE       */
    HTSYSMENU,     /* APP_ZONE_SYSMENU     */
};

static void
app_window_start_move( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        PostMessageW( win->hwnd, APP_WM_START_MOVE, 0, 0 );
}

static void
app_window_start_resize( win_id_t id, app_win_zone_t zone )
{
    app_window_t* win = win_get( id );
    if ( !win || !win->hwnd )
        return;
    if ( zone <= APP_ZONE_NONE || zone >= APP_ZONE_COUNT )
        return;
    PostMessageW( win->hwnd, APP_WM_START_RESIZE, ( WPARAM )win_zone_to_win32[ zone ], 0 );
}

static void
app_window_title_event( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        PostMessageW( win->hwnd, APP_WM_TITLE_EVENT, 0, 0 );
}

static void
app_window_system_menu( win_id_t id, i32 x, i32 y )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        PostMessageW( win->hwnd, APP_WM_SYSMENU, 0, ( LPARAM )MAKELONG( ( i16 )x, ( i16 )y ) );
}

static void
app_window_enable_resize( win_id_t id, bool enabled )
{
    app_window_t* win = win_get( id );
    if ( !win || !win->hwnd )
        return;

    win->has_resize = enabled;

    DWORD style = ( DWORD )GetWindowLongW( win->hwnd, GWL_STYLE );
    if ( enabled ) style |= WS_THICKFRAME;
    else           style &= ~WS_THICKFRAME;
    SetWindowLongW( win->hwnd, GWL_STYLE, style );

    /* Apply the frame change without moving / resizing / restacking the window. */
    SetWindowPos( win->hwnd, NULL, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
}

/* Publish the border-resize grab thickness imgui measures each frame for a native-borderless window.
   border is the edge-grab thickness in client px (<= 0 disables resize).  Toggling enabled changes
   the non-client layout, so that case forces a WM_NCCALCSIZE recompute; metric-only updates do not.
   imgui dispatches move / title / system-menu gestures itself via window_start_move etc., so no
   caption_h or holes are needed here. */
static void
app_window_set_native_frame( win_id_t id, bool enabled, i32 border )
{
    app_window_t* win = win_get( id );
    if ( !win || !win->hwnd )
        return;

    bool was = win->native.enabled;
    win->native.enabled = enabled;
    win->native.border  = border > 0 ? border : 0;

    if ( was != enabled )
        SetWindowPos( win->hwnd, NULL, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
}

/* Request a graceful close (as if the user clicked the OS close button): post WM_CLOSE so the
   normal close path runs -- main window sets the quit flag, an imgui-owned floater is torn down
   through its APP_EV_WIN_CLOSE handler.  Distinct from window_close, which destroys immediately. */
static void
app_window_request_close( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( win && win->hwnd )
        PostMessageW( win->hwnd, WM_CLOSE, 0, 0 );
}



/*============================================================================================*/
