/*==============================================================================================

    engine/app/app.c — Unity build entry point for the app module.

    Inclusion order matters
    -----------------------
        1. Standard headers           (stdio)
        2. orb.h                      (types and macros)
        3. Platform headers           (windows.h, gated by OS_WINDOWS)
        4. mod_export.h               (mod_desc_t, get_api_fn)
        5. app.h                      (app_api_t definition + key/button enums)
        6. Platform backends          (win_input.c — input handlers and snapshot;
                                       win_window_proc.c — WndProc uses those handlers;
                                       win_fiber.c — fiber pump, guarded by APP_WIN_FIBER;
                                       win_window.c — pool helpers, API impls, pump_events;
                                       win_lifecycle.c — fillscreen and paint toggle)
        7. app_api.c                  (assigns every static function to
                                       g_app_api_struct)

==============================================================================================*/

#include <stdio.h>
#include <stdarg.h>
#include "orb.h"

// clang-format off
/*==============================================================================================
    Platform headers
==============================================================================================*/

#if OS_WINDOWS

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN

    #include <windows.h>

    /* Enable fiber-based message pump so the game loop keeps ticking during
       resize / move / menu modal loops inside DefWindowProc.
       Comment this out to fall back to a direct PeekMessage poll. */
    #define APP_WIN_FIBER

    /* Timer ID used by the message fiber to re-enter the main fiber. */
    #define APP_FIBER_TIMER_ID 1

    /* Private window messages: the native-borderless action primitives POST these (non-blocking,
       safe from the game-loop fiber) so the actual WM_NCLBUTTONDOWN / TrackPopupMenu -- each of
       which runs its own blocking modal loop -- is handled inside the WndProc on the MESSAGE fiber.
       That is the only context where the fiber timer can yield to the game loop, so rendering keeps
       ticking during the drag (calling SendMessage from the game-loop fiber would deadlock it). */
    #define APP_WM_START_MOVE   ( WM_APP + 0 )
    #define APP_WM_START_RESIZE ( WM_APP + 1 ) /* wParam = Win32 HT* hit-test code */
    #define APP_WM_TITLE_EVENT  ( WM_APP + 2 )
    #define APP_WM_SYSMENU      ( WM_APP + 3 ) /* lParam = MAKELPARAM(client x, client y) */

#else

    #error "app: platform not implemented"

#endif

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "engine/mod/mod_export.h"
#include "engine/app/app_host.h"

/*==============================================================================================
    Log Sink

    g_app_log_fn routes window and event messages through core once wired by the host.
    Falls back to stdio when NULL (before core is live, or in tests that don't wire it).
==============================================================================================*/

static log_fn_t g_app_log_fn = NULL;

void
app_set_log_fn( log_fn_t fn )
{
    g_app_log_fn = fn;
}

static void
app_log( int level, const char* fmt, ... )
{
    char    buf[ 256 ];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    if ( g_app_log_fn )
         g_app_log_fn( level, "app", buf );
    else
        fprintf( level >= ORB_LOG_WARN ? stderr : stdout, "[app] %s\n", buf );
}

/*==============================================================================================
    Platform backend  (static functions appear in this TU)
==============================================================================================*/

#if OS_WINDOWS

typedef struct win_fillscreen_s
{
    bool            is_enabled;
    bool            prev_maximized;
    DWORD           prev_style;
    DWORD           prev_exstyle;
    i32             prev_w, prev_h;
    i32             prev_x, prev_y;

} win_fillscreen_t;

typedef struct app_window_s
{
    win_id_t             id;
    HWND                hwnd;
    app_win_state_t     state;
    app_win_state_t     prev;

    app_cursor_t        cursor;

    i32                 w, h;           /* cached client dimensions — valid even when minimized */
    bool                hover_tracking; /* TrackMouseEvent is armed                              */
    bool                resize_modal;   /* inside WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE loop        */

    bool                move_modal;
    bool                has_resize;    /* native sizing frame present (borderless windows)     */
    win_fillscreen_t    fill;

    /* Native-borderless custom frame (window kind 3).  When enabled, WM_NCCALCSIZE removes the
       visual non-client frame (client fills the whole window) and WM_NCHITTEST returns HTCLIENT
       everywhere except the edge-resize band, so gui owns the entire caption area.  Move /
       resize / Aero Snap / system-menu route through the window_start_move / window_start_resize /
       window_title_event / window_system_menu dispatch primitives.  WS_THICKFRAME keeps the
       sizing loop live; border is republished each frame by gui via window_set_native_frame. */
    struct
    {
        bool enabled;
        i32  border;      /* resize grab thickness at the edges, client px  (0 = no resize)  */

    } native;

} app_window_t;

typedef struct win_pool_s
{
    app_window_t    wins[ APP_WIN_MAX ];
    u32             alloc; /* bitmask: bit i = 1 → slot i is in use */
    win_id_t        main_id;
    ATOM            class_atom;
    HINSTANCE       hinst;

#ifdef APP_WIN_FIBER

    LPVOID          fiber_main;    /* game-loop fiber (ConvertThreadToFiber) */
    LPVOID          fiber_message; /* message-drain fiber (CreateFiber)      */

#endif

} win_pool_t;

static win_pool_t g_pool = { .main_id = APP_WIN_INVALID };

    #include "engine/app/win/win_input.c"
    #include "engine/app/win/win_window_proc.c"
    #ifdef APP_WIN_FIBER
    #include "engine/app/win/win_fiber.c"
    #endif
    #include "engine/app/win/win_window.c"
    #include "engine/app/win/win_lifecycle.c"

#endif

// clang-format on
/*==============================================================================================
    API Definition (must be last)
==============================================================================================*/

#ifndef APP_API_C_PRELUDE
#include "engine/app/app_api.c"
#endif

