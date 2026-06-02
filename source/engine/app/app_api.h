#ifndef APP_API_H
#define APP_API_H
/*==============================================================================================

    engine/app/app_api.h — app module API struct and gateway macro.

    Consumers call app()->pump_events() etc.

==============================================================================================*/

#include "engine/app/app.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct app_api_s
{
    /* ---- Window ---- */

    /* Open a native window. Returns APP_WIN_INVALID on failure.
       x = y = 0 -> OS centers; w = h = 0 -> 50% of desktop work area. */
    win_id_t ( *window_open )( const char* title, i32 x, i32 y, i32 w, i32 h, u32 flags );

    /* Destroy a window and release its OS resources. */
    void ( *window_close )( win_id_t id );

    bool            ( *window_is_valid          )( win_id_t id );
    void*           ( *window_handle            )( win_id_t id ); /* HWND on Windows */
    bool            ( *window_is_minimized      )( win_id_t id );
    app_win_state_t ( *window_state             )( win_id_t id );
    void            ( *window_set_fillscreen    )( win_id_t id, bool enabled );
    void            ( *window_toggle_fillscreen )( win_id_t id );

    /* Default OS background paint/erase. When enabled, Windows fills the client
       area with the registered class brush on WM_ERASEBKGND. Disable once a
       renderer owns the window's pixels — leaving it on causes flicker. */
    void ( *window_set_paint    )( win_id_t id, bool enabled );
    void ( *window_toggle_paint )( win_id_t id );
    bool ( *window_paint_enabled)( win_id_t id );

    /* ---- Event loop ---- */

    /* Drain the OS message queue, snapshot input state, fill the event ring buffer.
       Returns false when the application should exit. */
    bool ( *pump_events )( void );

    /* Pull the next typed event from the ring buffer. Returns false when empty. */
    bool ( *next_event  )( app_event_t* out );

    bool ( *should_quit )( void );

    /* ---- Input snapshot ---- */

    bool ( *key_down     )( app_key_t key );
    bool ( *key_pressed  )( app_key_t key );
    bool ( *key_released )( app_key_t key );

    void ( *mouse_position        )( i32* out_x, i32* out_y );
    bool ( *mouse_button_down     )( app_mouse_button_t btn );
    bool ( *mouse_button_pressed  )( app_mouse_button_t btn );
    bool ( *mouse_button_released )( app_mouse_button_t btn );

    /* Enable key-repeat events (text mode). Default false = game mode (repeats suppressed). */
    void ( *key_repeat_set )( bool enabled );

} app_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( APP_STATIC )
    MOD_GATEWAY_STATIC( app_api_t, app )
#else
    MOD_GATEWAY_DYNAMIC( app_api_t, app )
#endif

#if defined( BUILD_STATIC ) || defined( APP_STATIC )
    #define MOD_USE_APP    /* static build */
    #define MOD_FETCH_APP  true
#else
    #define MOD_USE_APP    MOD_DEFINE_API_PTR( app_api_t, app )
    #define MOD_FETCH_APP  MOD_FETCH_API( app_api_t, app )
#endif

// clang-format on
/*============================================================================================*/
#endif    // APP_API_H
