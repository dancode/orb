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

            if ( repeat )
            {
                if ( !g_key_repeat )
                    return true; /* game mode: suppress repeats */

                /* Text mode: synthesize an up so key_pressed fires again. */
                input_handle_key_up( wp, lp, win->id );
            }

            input_handle_key_down( wp, lp, win->id );
            return true;
        }

        case WM_SYSKEYUP:
        case WM_KEYUP:
            input_handle_key_up( wp, lp, win->id );
            return true;

        case WM_CHAR:
        {
            bool repeat = ( HIWORD( lp ) & KF_REPEAT ) != 0;
            if ( repeat && !g_key_repeat )
                return true; /* suppress repeated chars in game mode */

            input_handle_char( wp, win->id );
            return true;
        }

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
        }
            return 0;

        case WM_MOUSEHOVER: win->state.hovered = 1; return 0;

        case WM_MOUSELEAVE:
            win->state.hovered  = 0;
            win->hover_tracking = false; /* one-shot — must re-arm next WM_MOUSEMOVE */
            return 0;

        case WM_ERASEBKGND:
            /* paint_enabled = true: let DefWindowProcW fill with the class brush.
               paint_enabled = false: renderer owns the pixels — suppress to prevent flicker. */
            if ( !win->paint_enabled )
                return 1;
            return DefWindowProcW( hwnd, msg, wp, lp );

        default: return DefWindowProcW( hwnd, msg, wp, lp );
    }
}

/*============================================================================================*/
