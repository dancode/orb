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

/*----------------------------------------------------------------------------------------------
    Paint enable / toggle / query — controls default OS background erase.
----------------------------------------------------------------------------------------------*/

static void
app_window_set_paint( win_id_t id, bool enabled )
{
    app_window_t* win = win_get( id );
    if ( !win || win->paint_enabled == enabled )
        return;
    win->paint_enabled = enabled;
    /* Force a repaint so the new erase policy is visible immediately. */
    if ( win->hwnd )
        InvalidateRect( win->hwnd, NULL, TRUE );
}

static void
app_window_toggle_paint( win_id_t id )
{
    app_window_t* win = win_get( id );
    if ( !win )
        return;
    app_window_set_paint( id, !win->paint_enabled );
}

static bool
app_window_paint_enabled( win_id_t id )
{
    app_window_t* win = win_get( id );
    return win ? win->paint_enabled : false;
}

/*============================================================================================*/
