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

    /* Screen-space geometry.  window_get_pos returns the window's CLIENT-area top-left in
       virtual-desktop screen coordinates; window_set_pos moves the window so its CLIENT corner
       lands at the given screen point (frame offset handled internally).  Paired with
       mouse_position_screen (below) these let a multi-window UI place / track a torn-off OS window
       at an exact screen location, independent of which window the cursor is over. */
    void            ( *window_get_pos           )( win_id_t id, i32* out_x, i32* out_y );
    void            ( *window_set_pos           )( win_id_t id, i32 x, i32 y );

    app_win_state_t ( *window_state             )( win_id_t id );
    void            ( *window_set_fillscreen    )( win_id_t id, bool enabled );
    void            ( *window_toggle_fillscreen )( win_id_t id );

    /* Programmatic geometry / show-state control.  Each routes through the OS so the
       normal WM_SIZE path runs: window state is updated and an APP_EV_WIN_RESIZE is
       posted, exactly as for a user-driven resize.  window_resize takes a CLIENT size
       (the drawable area), restoring the window first if it is minimized/maximized so
       the new size takes effect.  Sizes <= 0 are ignored. */
    void ( *window_resize   )( win_id_t id, i32 w, i32 h );
    void ( *window_minimize )( win_id_t id );
    void ( *window_restore  )( win_id_t id );

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

    /* key_pressed is the initial press only; key_pressed_repeat also fires on each OS auto-repeat
       tick (at the user's system rate).  Per-query, so there is no input mode to set: game-style
       actions read key_pressed (one press per physical key), text / nav reads key_pressed_repeat. */
    bool ( *key_down            )( app_key_t key );
    bool ( *key_pressed         )( app_key_t key );
    bool ( *key_pressed_repeat  )( app_key_t key );
    bool ( *key_released        )( app_key_t key );

    void ( *mouse_position        )( i32* out_x, i32* out_y );
    void ( *mouse_position_screen )( i32* out_x, i32* out_y );  /* absolute desktop screen coords */
    bool ( *mouse_button_down     )( app_mouse_button_t btn );
    bool ( *mouse_button_pressed  )( app_mouse_button_t btn );
    bool ( *mouse_button_released )( app_mouse_button_t btn );

    /* ---- Clipboard ---- */

    /* Copy NUL-terminated `text` to the OS clipboard (the outbound half: cut / copy).
       The inbound half (paste) is delivered as an APP_EV_CLIPBOARD event when the user
       presses the paste gesture, so reading the clipboard needs no polling API. */
    void ( *clipboard_set )( const char* text );

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
