#ifndef APP_H
#define APP_H
/*==============================================================================================

    engine/app/app_api.h — App module, public types and gateway.

    Single header following the same pattern as engine/core/core.h: this file
    exposes only the types consumers need (the app_api_t struct slot signatures
    and the gateway accessor). Every implementation function is static inside
    app.c's unity build and reachable exclusively through app_api()->...

    Scope (v0)
    ----------
    - Window: create, destroy, pump, native handle
    - Keyboard input: down / pressed (this frame) / released (this frame)
    - Mouse input: position, button down / pressed / released

    Not in v0: text input, mouse wheel, gamepad, raw input, IME, modifier
    combos, multi-window, fullscreen, DPI awareness, resize callbacks.

    Frame semantics for input
    -------------------------
    Input state snapshots at the top of pump_events(). After pump_events()
    returns, queries reflect the latest message-drained state for THIS frame.
    pressed/released report transitions between the previous frame's snapshot
    and this frame's snapshot. A key tapped and released within a single
    16ms frame can therefore be missed — acceptable for v0 game input.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod_api.h"

/*==============================================================================================
    Input — Key codes
==============================================================================================*/
/* clang-format off */

typedef enum app_key_e
{
    APP_KEY_NONE = 0,

    APP_KEY_A, APP_KEY_B, APP_KEY_C, APP_KEY_D,
    APP_KEY_E, APP_KEY_F, APP_KEY_G, APP_KEY_H,
    APP_KEY_I, APP_KEY_J, APP_KEY_K, APP_KEY_L,
    APP_KEY_M, APP_KEY_N, APP_KEY_O, APP_KEY_P,
    APP_KEY_Q, APP_KEY_R, APP_KEY_S, APP_KEY_T,
    APP_KEY_U, APP_KEY_V, APP_KEY_W, APP_KEY_X,
    APP_KEY_Y, APP_KEY_Z,

    APP_KEY_0, APP_KEY_1, APP_KEY_2, APP_KEY_3, APP_KEY_4,
    APP_KEY_5, APP_KEY_6, APP_KEY_7, APP_KEY_8, APP_KEY_9,

    APP_KEY_F1,  APP_KEY_F2,  APP_KEY_F3,  APP_KEY_F4,
    APP_KEY_F5,  APP_KEY_F6,  APP_KEY_F7,  APP_KEY_F8,
    APP_KEY_F9,  APP_KEY_F10, APP_KEY_F11, APP_KEY_F12,

    APP_KEY_ESCAPE, APP_KEY_ENTER, APP_KEY_SPACE,
    APP_KEY_TAB,    APP_KEY_BACKSPACE,

    APP_KEY_LEFT, APP_KEY_RIGHT, APP_KEY_UP, APP_KEY_DOWN,

    APP_KEY_SHIFT, APP_KEY_CONTROL, APP_KEY_ALT,

    APP_KEY_COUNT

} app_key_t;

/* clang-format on */

/*==============================================================================================
    Input — Mouse buttons
==============================================================================================*/
typedef enum app_mouse_button_e
{
    APP_MOUSE_LEFT   = 0,
    APP_MOUSE_RIGHT  = 1,
    APP_MOUSE_MIDDLE = 2,

    APP_MOUSE_BUTTON_COUNT

} app_mouse_button_t;

/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct app_api_s
{
    /* Create a window with the given client-area dimensions. Title is UTF-8.
       Returns false if window class registration or creation fails. */
    bool ( *window_create )( const char* title, int width, int height );

    /* Destroy the window and unregister its class. Safe to call if never created. */
    void ( *window_destroy )( void );

    /* Drain pending OS messages. Returns false once WM_QUIT has been seen —
       hosts return false from on_update on that signal to exit the runtime loop. */
    bool ( *pump_events )( void );

    /* Native window handle (HWND on Windows). Cast at the renderer call site.
       Returns NULL if the window has not been created. */
    void* ( *window_handle )( void );

    /* ---- Keyboard ---- */

    /* Currently held down */
    bool ( *key_down )( app_key_t key );
    /* Transitioned up to down this frame */
    bool ( *key_pressed )( app_key_t key );
    /* Transitioned down to up this frame */
    bool ( *key_released )( app_key_t key );

    /* ---- Mouse ---- */

    /* Client-area pixel coordinates of the cursor. Either output pointer may be NULL. */
    void ( *mouse_position )( i32* out_x, i32* out_y );

    bool ( *mouse_button_down )( app_mouse_button_t btn );
    bool ( *mouse_button_pressed )( app_mouse_button_t btn );
    bool ( *mouse_button_released )( app_mouse_button_t btn );

} app_api_t;

#if defined( BUILD_STATIC ) || defined( APP_STATIC )
MOD_GATEWAY_STATIC( app_api_t, app )
#else
MOD_GATEWAY_DYNAMIC( app_api_t, app )
#endif

/*============================================================================================*/
#endif    // APP_H