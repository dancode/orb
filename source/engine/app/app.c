/*==============================================================================================

    engine/app/app.c — Unity build entry point for the app module.

    Inclusion order matters
    -----------------------
        1. Standard headers           (stdio)
        2. orb.h                      (types and macros)
        3. Platform headers           (windows.h, gated by OS_WINDOWS)
        4. mod_export.h               (mod_api_t, get_api_fn)
        5. app.h                      (app_api_t definition + key/button enums)
        6. Platform backends          (win_input.c first — defines handlers
                                       and snapshot; win_window.c second — its
                                       WndProc calls those handlers, and
                                       pump_events calls input_snapshot)
        7. app_api.c                  (assigns every static function to
                                       g_app_api_struct)

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

/*==============================================================================================
    Platform headers
==============================================================================================*/

#if OS_WINDOWS

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN

    #include <windows.h>

#else

    #error "app: platform not implemented"

#endif

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "engine/mod/mod_export.h"
#include "engine/app/app.h"

/*==============================================================================================
    Platform backend  (static functions appear in this TU)
==============================================================================================*/

#if OS_WINDOWS

typedef struct win_fillscreen_s
{
    bool  is_enabled;
    bool  prev_maximized;
    DWORD prev_style;
    DWORD prev_exstyle;
    i32   prev_w, prev_h;
    i32   prev_x, prev_y;

} win_fillscreen_t;

typedef struct app_window_s
{
    win_id_t        id;
    HWND            hwnd;
    app_win_state_t state;
    app_win_state_t prev;

    i32  w, h;                  /* cached client dimensions — valid even when minimized */
    bool hover_tracking;        /* TrackMouseEvent is armed                              */
    bool resize_modal;          /* inside WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE loop        */
    bool move_modal;
    win_fillscreen_t fill;

} app_window_t;

typedef struct win_pool_s
{
    app_window_t wins[ APP_WIN_MAX ];
    u32          alloc; /* bitmask: bit i = 1 → slot i is in use */
    win_id_t     main_id;
    ATOM         class_atom;
    HINSTANCE    hinst;

} win_pool_t;

static win_pool_t g_pool;

    #include "engine/app/win/win_input.c"
    #include "engine/app/win/win_window_proc.c"
    #include "engine/app/win/win_window.c"
    #include "engine/app/win/win_lifecycle.c"

#endif

/*==============================================================================================
    API wiring  (must be last)
==============================================================================================*/

#include "engine/app/app_api.c"

/*============================================================================================*/


/*============================================================================================*/