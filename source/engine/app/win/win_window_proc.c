/*==============================================================================================
    Window procedure
==============================================================================================*/

/* win_set_fillscreen is defined in win_lifecycle.c, included after this file. */
static void win_set_fillscreen( app_window_t* win, bool enabled );

/* clang-format off */
static bool
win_proc_mouse( app_window_t* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    /* Extract coords once — all mouse messages except MOUSEWHEEL use client coords. */
    i16 x = ( i16 )LOWORD( lp );
    i16 y = ( i16 )HIWORD( lp );

    if ( msg == WM_MOUSEWHEEL )
    {
        /* MOUSEWHEEL lparam is in screen coordinates — convert to client. */
        POINT pt = { ( i32 )x, ( i32 )y };
        MapWindowPoints( NULL, hwnd, &pt, 1 );
        x = ( i16 )pt.x;
        y = ( i16 )pt.y;
    }

    switch ( msg )
    {
        case WM_MOUSEMOVE:
            {
                /* Capture directs all mouse input here while any button is held,
                   even when the cursor leaves the client area (e.g. viewport drag). */
                u32 btns = MK_LBUTTON | MK_MBUTTON | MK_RBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
                if ( wp & btns )
                {
                    if ( !win->state.captured )
                    {
                        win->state.captured = 1;
                        SetCapture( hwnd );
                    }
                }
                else if ( win->state.captured )
                {
                    win->state.captured = 0;
                    ReleaseCapture();
                }

                /* Arm hover/leave tracking on each entry — TrackMouseEvent is one-shot. */
                if ( !win->hover_tracking )
                {
                    TRACKMOUSEEVENT tme = {
                        .cbSize      = sizeof( tme ),
                        .dwFlags     = TME_HOVER | TME_LEAVE,
                        .hwndTrack   = hwnd,
                        .dwHoverTime = 1,
                    };
                    TrackMouseEvent( &tme );
                    win->hover_tracking = true;
                }

                input_handle_mouse_move( x, y, win->id );
            }
            return true;
        case WM_MOUSEWHEEL:  input_handle_mouse_wheel( wp, x, y, win->id );                        return true;
        case WM_LBUTTONDOWN: input_handle_mouse_button( APP_MOUSE_LEFT,   true,  x, y, win->id );  return true;
        case WM_LBUTTONUP:   input_handle_mouse_button( APP_MOUSE_LEFT,   false, x, y, win->id );  return true;
        case WM_RBUTTONDOWN: input_handle_mouse_button( APP_MOUSE_RIGHT,  true,  x, y, win->id );  return true;
        case WM_RBUTTONUP:   input_handle_mouse_button( APP_MOUSE_RIGHT,  false, x, y, win->id );  return true;
        case WM_MBUTTONDOWN: input_handle_mouse_button( APP_MOUSE_MIDDLE, true,  x, y, win->id );  return true;
        case WM_MBUTTONUP:   input_handle_mouse_button( APP_MOUSE_MIDDLE, false, x, y, win->id );  return true;
        case WM_XBUTTONDOWN: input_handle_mouse_button( HIWORD( wp ) == XBUTTON1 ? APP_MOUSE_X1 : APP_MOUSE_X2, true,  x, y, win->id ); return true;
        case WM_XBUTTONUP:   input_handle_mouse_button( HIWORD( wp ) == XBUTTON1 ? APP_MOUSE_X1 : APP_MOUSE_X2, false, x, y, win->id ); return true;
        default:             return false;
    }
}

/* clang-format on */

static bool
win_proc_keyboard( app_window_t* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    UNUSED( hwnd );
    switch ( msg )
    {
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
        {
            bool alt    = ( HIWORD( lp ) & KF_ALTDOWN ) != 0;
            bool repeat = ( HIWORD( lp ) & KF_REPEAT  ) != 0;

            /* Alt+F4 — let DefWindowProcW generate WM_CLOSE normally. */
            if ( alt && wp == VK_F4 )
                return false;

            /* Alt+Enter — toggle fillscreen. */
            if ( alt && wp == VK_RETURN )
            {
                win_set_fillscreen( win, !win->fill.is_enabled );
                return true;
            }

            /* Paste gesture (Ctrl+V or Shift+Insert): the platform owns the paste keybinding,
               reads the OS clipboard, and posts it as an APP_EV_CLIPBOARD event for consumers
               to apply.  Copy / cut stay with the consumer (only it knows the selection) and
               travel back out through app_clipboard_set.  The key-down event still fires below,
               so callers that watch raw keys are unaffected. */
            {
                bool ctrl  = ( GetKeyState( VK_CONTROL ) & 0x8000 ) != 0;
                bool shift = ( GetKeyState( VK_SHIFT )   & 0x8000 ) != 0;
                if ( ( ctrl && wp == 'V' ) || ( shift && wp == VK_INSERT ) )
                    input_handle_paste( win->id );
            }

            /* Always deliver the key-down, tagging OS auto-repeats.  The repeat feeds only
               key_pressed_repeat (and an event with repeat=1); a consumer reading key_pressed
               sees one press per physical key.  No synthesized key-up -- the key never went up,
               so reporting a release would be a lie to consumers. */
            input_handle_key_down( wp, lp, repeat, win->id );
            return true;
        }

        case WM_SYSKEYUP:
        case WM_KEYUP:
            input_handle_key_up( wp, lp, win->id );
            return true;

        case WM_CHAR:
            /* Deliver every char, including OS auto-repeats -- a focused text consumer wants held-
               key repeat; anything not editing text simply ignores CHAR events. */
            input_handle_char( wp, win->id );
            return true;

        case WM_SYSCOMMAND:
        case WM_COMMAND:
            if ( wp == SC_KEYMENU )
            {
                /* lp holds the alt+key character — post it as a char event so
                   callers can observe which alt+key combination was pressed. */
                if ( lp )
                {
                    app_event_t ev         = win_make_event( APP_EV_CHAR, win->id );
                    ev.data.text.codepoint = ( u32 )lp;
                    ev.data.text.mod       = win_get_mod();
                    win_post_event( &ev );
                }
                return true;
            }
            return false;

        default: return false;
    }
}

static LRESULT CALLBACK
app_wnd_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    app_window_t* win = ( app_window_t* )GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if ( !win )
        return DefWindowProcW( hwnd, msg, wp, lp );

    /* Mouse messages are the most frequent — check first. */
    if ( msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST )
        return win_proc_mouse( win, hwnd, msg, wp, lp ) ? 0 : DefWindowProcW( hwnd, msg, wp, lp );

    if ( ( msg >= WM_KEYFIRST && msg <= WM_KEYLAST ) || ( msg == WM_SYSCOMMAND || msg == WM_COMMAND ) )
        return win_proc_keyboard( win, hwnd, msg, wp, lp ) ? 0 : DefWindowProcW( hwnd, msg, wp, lp );

    switch ( msg )
    {
        case WM_CLOSE:
        {
            app_event_t ev = win_make_event( APP_EV_WIN_CLOSE, win->id );
            win_post_event( &ev );
            if ( win->id == g_pool.main_id )
                g_app_quit = true;
        }
            return 0; /* do NOT pass to DefWindowProcW — that would call DestroyWindow */

        case WM_SETFOCUS:
            win->state.focused = 1;
            {
                app_event_t ev = win_make_event( APP_EV_WIN_FOCUS, win->id );
                win_post_event( &ev );
            }
            return 0;

        case WM_KILLFOCUS:
            win->state.focused = 0;
            input_clear_all_state();
            {
                app_event_t ev = win_make_event( APP_EV_WIN_BLUR, win->id );
                win_post_event( &ev );
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            win->resize_modal = true;
            win->move_modal   = true;
            return 0;

        case WM_EXITSIZEMOVE:
            win->resize_modal = false;
            win->move_modal   = false;
            return 0;

        case WM_SIZE:
        {
            if ( wp == SIZE_MINIMIZED )
            {
                win->state.minimized = 1;
                win->state.maximized = 0;
                win->state.restored  = 0;
                win->state.hidden    = 0;
            }
            else if ( wp == SIZE_MAXIMIZED )
            {
                win->state.minimized = 0;
                win->state.maximized = 1;
                win->state.restored  = 0;
            }
            else
            {
                win->state.minimized = 0;
                win->state.maximized = 0;
                win->state.restored  = 1;
            }

            /* Cache dimensions — don't update when minimized (lParam is 0). */
            if ( wp != SIZE_MINIMIZED )
            {
                win->w = ( i32 )LOWORD( lp );
                win->h = ( i32 )HIWORD( lp );
            }

            app_event_t ev       = win_make_event( APP_EV_WIN_RESIZE, win->id );
            ev.data.win_resize.w = win->w;
            ev.data.win_resize.h = win->h;
            win_post_event( &ev );

#ifdef APP_WIN_FIBER
            /* Yield synchronously to the main game fiber so it processes the resize event
               and renders/presents the new swapchain frame before the OS composites it. */
            if ( g_pool.fiber_main && GetCurrentFiber() != g_pool.fiber_main )
            {
                SwitchToFiber( g_pool.fiber_main );
            }
#endif
        }
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint( hwnd, &ps );
#ifdef APP_WIN_FIBER
            /* Force a synchronous game frame update and present during OS repaint requests. */
            if ( g_pool.fiber_main && GetCurrentFiber() != g_pool.fiber_main )
            {
                SwitchToFiber( g_pool.fiber_main );
            }
#endif
            EndPaint( hwnd, &ps );
        }
            return 0;

        case WM_MOUSEHOVER: win->state.hovered = 1; return 0;

        case WM_MOUSELEAVE:
            win->state.hovered  = 0;
            win->hover_tracking = false; /* one-shot — must re-arm next WM_MOUSEMOVE */
            return 0;

        case WM_ERASEBKGND:
            /* The renderer owns the client pixels — suppress default OS erase to prevent flicker. */
            return 1;

        case WM_NCCALCSIZE:
            /* Custom frame: claim the whole window rect as client so no visual non-client frame
               remains.  Returning 0 with rgrc[0] unchanged does exactly that.  When maximized the
               proposed rect overhangs the monitor by the frame thickness, so inset the client by it
               -- otherwise the top row and the taskbar edge are clipped. */
            if ( win->native.enabled && wp == TRUE )
            {
                NCCALCSIZE_PARAMS* p = ( NCCALCSIZE_PARAMS* )lp;
                if ( IsZoomed( hwnd ) )
                {
                    int fx = GetSystemMetrics( SM_CXFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER );
                    int fy = GetSystemMetrics( SM_CYFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER );
                    p->rgrc[ 0 ].left   += fx;
                    p->rgrc[ 0 ].right  -= fx;
                    p->rgrc[ 0 ].top    += fy;
                    p->rgrc[ 0 ].bottom -= fy;
                }
                return 0;
            }
            return DefWindowProcW( hwnd, msg, wp, lp );

        case WM_NCHITTEST:
        {
            /* Report edge / corner resize zones so the OS runs the matching native loop; return
               HTCLIENT everywhere else.  imgui owns the entire caption band -- when the user grabs
               the title bar imgui dispatches window_start_move (which posts APP_WM_START_MOVE below),
               so OS move / Aero Snap / double-click-maximize / system-menu still work, but imgui
               sees all clicks first and can run its own caption widgets without holes.
               Non-custom windows keep default behavior. */
            if ( !win->native.enabled )
                return DefWindowProcW( hwnd, msg, wp, lp );

            POINT pt = { ( i16 )LOWORD( lp ), ( i16 )HIWORD( lp ) }; /* screen coords */
            ScreenToClient( hwnd, &pt );                            /* client == window rect now */

            RECT rc;
            GetClientRect( hwnd, &rc );
            i32 w = rc.right, h = rc.bottom;
            i32 b = win->native.border;

            /* No edge resize while maximized (the borders are off-screen / pinned). */
            bool can_size = b > 0 && !IsZoomed( hwnd );
            bool l = pt.x < b, r = pt.x >= w - b, t = pt.y < b, bot = pt.y >= h - b;

            if ( can_size )
            {
                if ( t && l )   return HTTOPLEFT;
                if ( t && r )   return HTTOPRIGHT;
                if ( bot && l ) return HTBOTTOMLEFT;
                if ( bot && r ) return HTBOTTOMRIGHT;
                if ( l )        return HTLEFT;
                if ( r )        return HTRIGHT;
                if ( t )        return HTTOP;
                if ( bot )      return HTBOTTOM;
            }

            return HTCLIENT;
        }

        /* Native-borderless actions, posted by the app_window_* primitives from the game-loop
           fiber and handled here on the message fiber so their blocking modal loops keep the
           game loop rendering (the fiber timer can only yield to it from this context).
           ReleaseCapture first -- WM_NCLBUTTONDOWN is ignored while the window holds the mouse
           capture, which the WndProc takes on button-held in win_proc_mouse. */
        case APP_WM_START_MOVE:
            ReleaseCapture();
            win->state.captured = 0;
            SendMessageW( hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0 );
            return 0;

        case APP_WM_START_RESIZE:
            ReleaseCapture();
            win->state.captured = 0;
            SendMessageW( hwnd, WM_NCLBUTTONDOWN, wp /* HT* code */, 0 );
            return 0;

        case APP_WM_TITLE_EVENT:
            /* Native caption double-click: the OS toggles maximize / restore. */
            ReleaseCapture();
            win->state.captured = 0;
            SendMessageW( hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION, 0 );
            return 0;

        case APP_WM_SYSMENU:
        {
            HMENU sys_menu = GetSystemMenu( hwnd, FALSE );
            if ( !sys_menu )
                return 0;

            /* Client click -> screen position for the popup. */
            POINT pos = { ( i16 )LOWORD( lp ), ( i16 )HIWORD( lp ) };
            ClientToScreen( hwnd, &pos );

            int cmd = ( int )TrackPopupMenu( sys_menu, TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD,
                                             pos.x, pos.y, 0, hwnd, NULL );

            /* Windows cannot transition out of fillscreen on its own; leave it first. */
            if ( win->fill.is_enabled )
                win_set_fillscreen( win, false );

            if ( cmd > 0 )
                SendMessageW( hwnd, WM_SYSCOMMAND, ( WPARAM )cmd, 0 );
        }
            return 0;

        default: return DefWindowProcW( hwnd, msg, wp, lp );
    }
}

/*============================================================================================*/
