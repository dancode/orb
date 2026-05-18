#ifndef APP_H
#define APP_H
/*==============================================================================================

    engine/app/app.h — Application layer: windowing, events, input snapshot.

    Window lifecycle
    ----------------
    window_open() allocates a slot from a fixed pool and returns a win_id_t.
    The first window opened becomes the main window; closing it sets the quit
    flag. Pass the id to window_handle() to retrieve the platform handle (HWND
    on Windows) needed by the renderer or other platform-aware subsystems.

    Event loop
    ----------
    pump_events() drains the OS message queue, snapshots input state, fills the
    internal ring buffer with typed events, and returns false when the application
    should quit. Call next_event() in a loop after each pump to pull and process
    queued events in order.

    Input snapshot
    --------------
    key_down / key_pressed / key_released and mouse equivalents provide direct
    frame-granularity queries without iterating events. The snapshot is taken at
    the top of each pump_events() call so that pressed/released reflect exactly
    the transitions between two consecutive frames.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"

/*==============================================================================================
    Window IDs and pool constants
==============================================================================================*/

typedef i32 win_id_t;

#define APP_WIN_INVALID ( -1 )
#define APP_WIN_MAX     4  /* max simultaneous native windows             */
#define APP_EVENT_MAX   64 /* ring buffer capacity — must be power of 2   */
#define APP_EVENT_MASK  ( APP_EVENT_MAX - 1 )

/*==============================================================================================
    Window creation flags
==============================================================================================*/

typedef enum app_win_flags_e
{
    APP_WIN_TITLE      = 1 << 0, /* title bar                                  */
    APP_WIN_RESIZE     = 1 << 1, /* resize border + min / max buttons          */
    APP_WIN_CLOSE      = 1 << 2, /* close button                               */
    APP_WIN_DEFAULT    = APP_WIN_TITLE | APP_WIN_RESIZE | APP_WIN_CLOSE,
    APP_WIN_POPUP      = 1 << 3, /* borderless popup — no chrome               */
    APP_WIN_TOPMOST    = 1 << 4, /* stays above all other windows              */
    APP_WIN_HIDDEN     = 1 << 5, /* created hidden — show manually             */
    APP_WIN_FILLSCREEN = 1 << 6, /* borderless fullscreen on current monitor   */
    APP_WIN_MAXIMIZE   = 1 << 7, /* start maximized                            */
    APP_WIN_MINIMIZE   = 1 << 8, /* start minimized                            */
    APP_WIN_NOFOCUS    = 1 << 9, /* do not steal focus on creation             */

} app_win_flags_t;

/*==============================================================================================
    Window state

    state  = current frame bits.
    prev   = previous frame bits.
    changed = state.bits XOR prev.bits — which flags flipped this frame.
==============================================================================================*/

typedef union app_win_state_u
{
    struct
    {
        u32 focused    : 1; /* window has OS input focus              */
        u32 minimized  : 1; /* window is iconic                       */
        u32 maximized  : 1; /* window is maximized                    */
        u32 restored   : 1; /* normal size — not min / max / fill     */
        u32 fillscreen : 1; /* borderless fullscreen mode             */
        u32 hovered    : 1; /* cursor is inside the client area       */
        u32 captured   : 1; /* mouse is captured (button held)        */
        u32 hidden     : 1; /* window is not visible                  */
    };

    u32 bits;

} app_win_state_t;

/*==============================================================================================
    Events
==============================================================================================*/

typedef enum app_event_type_e
{
    APP_EV_NONE = 0,

    APP_EV_KEY_DOWN,    // key pressed (no auto-repeat)
    APP_EV_KEY_UP,      // key released
    APP_EV_CHAR,        // printable Unicode codepoint (UTF-32)

    APP_EV_MOUSE_MOVE,
    APP_EV_MOUSE_DOWN,
    APP_EV_MOUSE_UP,
    APP_EV_MOUSE_WHEEL,

    APP_EV_WIN_FOCUS,     // window gained OS focus
    APP_EV_WIN_BLUR,      // window lost OS focus
    APP_EV_WIN_RESIZE,    // client area resized
    APP_EV_WIN_CLOSE,     // user triggered window close

    APP_EV_QUIT,    // application should exit

} app_event_type_t;

/*----------------------------------------------------------------------------------------------
    Modifier key state packed into 16 bits.
----------------------------------------------------------------------------------------------*/

typedef union app_mod_u
{
    struct
    {
        u16 ctrl  : 1;
        u16 shift : 1;
        u16 alt   : 1;
        u16 super : 1; /* Win key on Windows, Command on macOS */
    };

    u16 bits;

} app_mod_t;

/*----------------------------------------------------------------------------------------------
    Event payload sub-structs — each exactly 8 bytes so app_event_t is 32 bytes.
----------------------------------------------------------------------------------------------*/

typedef struct app_key_event_s /* 8 bytes */
{
    i32       key;   /* app_key_t                              */
    u8        press; /* 0 = released, 255 = pressed            */
    u8        pad;
    app_mod_t mod;

} app_key_event_t;

typedef struct app_text_event_s /* 8 bytes */
{
    u32       codepoint; /* UTF-32 character value             */
    app_mod_t mod;
    u16       pad;

} app_text_event_t;

typedef struct app_mouse_move_event_s /* 8 bytes */
{
    i16 x, y;   /* client-area cursor position this frame   */
    i16 dx, dy; /* delta from previous frame position        */

} app_mouse_move_event_t;

typedef struct app_mouse_btn_event_s /* 8 bytes */
{
    i32 button; /* app_mouse_button_t                        */
    i16 x, y;   /* client-area position at click             */

} app_mouse_btn_event_t;

typedef struct app_mouse_wheel_event_s /* 8 bytes */
{
    i32 delta; /* signed; positive = toward user            */
    i16 x, y;  /* client-area cursor position               */

} app_mouse_wheel_event_t;

typedef struct app_win_resize_event_s /* 8 bytes */
{
    i32 w, h; /* new client area dimensions                 */

} app_win_resize_event_t;

/*----------------------------------------------------------------------------------------------
    app_event_t — 32 bytes, ring-buffer friendly.
    event_id:   monotonic counter for ordering / replay.
    win_id:     window that received this event (APP_WIN_INVALID for app-level events).
    timestamp:  GetTickCount64() milliseconds.
----------------------------------------------------------------------------------------------*/

typedef struct app_event_s
{
    i32 event_id;
    i32 win_id;
    i32 type; /* app_event_type_t */
    i32 pad;
    i64 timestamp;

    union
    {
        app_key_event_t         key;
        app_text_event_t        text;
        app_mouse_move_event_t  mouse_move;
        app_mouse_btn_event_t   mouse_btn;
        app_mouse_wheel_event_t mouse_wheel;
        app_win_resize_event_t  win_resize;
    } data;

} app_event_t;

/*==============================================================================================
    Input — key codes
==============================================================================================*/

/* clang-format off */
typedef enum app_key_e
{
    APP_KEY_NONE = 0,

    /* Letters */
    APP_KEY_A, APP_KEY_B, APP_KEY_C, APP_KEY_D, APP_KEY_E, APP_KEY_F,
    APP_KEY_G, APP_KEY_H, APP_KEY_I, APP_KEY_J, APP_KEY_K, APP_KEY_L,
    APP_KEY_M, APP_KEY_N, APP_KEY_O, APP_KEY_P, APP_KEY_Q, APP_KEY_R,
    APP_KEY_S, APP_KEY_T, APP_KEY_U, APP_KEY_V, APP_KEY_W, APP_KEY_X,
    APP_KEY_Y, APP_KEY_Z,

    /* Row digits */
    APP_KEY_0, APP_KEY_1, APP_KEY_2, APP_KEY_3, APP_KEY_4,
    APP_KEY_5, APP_KEY_6, APP_KEY_7, APP_KEY_8, APP_KEY_9,

    /* Function keys */
    APP_KEY_F1,  APP_KEY_F2,  APP_KEY_F3,  APP_KEY_F4,
    APP_KEY_F5,  APP_KEY_F6,  APP_KEY_F7,  APP_KEY_F8,
    APP_KEY_F9,  APP_KEY_F10, APP_KEY_F11, APP_KEY_F12,

    /* Control */
    APP_KEY_ESCAPE, APP_KEY_ENTER, APP_KEY_SPACE, APP_KEY_TAB, APP_KEY_BACKSPACE,

    /* Arrow keys */
    APP_KEY_LEFT, APP_KEY_RIGHT, APP_KEY_UP, APP_KEY_DOWN,

    /* Navigation */
    APP_KEY_INSERT, APP_KEY_DELETE, APP_KEY_HOME, APP_KEY_END,
    APP_KEY_PAGE_UP, APP_KEY_PAGE_DOWN,

    /* Modifiers — left and right distinguished */
    APP_KEY_LSHIFT, APP_KEY_RSHIFT,
    APP_KEY_LCTRL,  APP_KEY_RCTRL,
    APP_KEY_LALT,   APP_KEY_RALT,
    APP_KEY_LSUPER, APP_KEY_RSUPER,   /* Win key on Windows, Command on macOS */

    /* Lock keys */
    APP_KEY_CAPS_LOCK, APP_KEY_NUM_LOCK, APP_KEY_SCROLL_LOCK,

    /* Numpad */
    APP_KEY_NP_0, APP_KEY_NP_1, APP_KEY_NP_2, APP_KEY_NP_3, APP_KEY_NP_4,
    APP_KEY_NP_5, APP_KEY_NP_6, APP_KEY_NP_7, APP_KEY_NP_8, APP_KEY_NP_9,
    APP_KEY_NP_ENTER, APP_KEY_NP_DOT,
    APP_KEY_NP_ADD, APP_KEY_NP_SUB, APP_KEY_NP_MUL, APP_KEY_NP_DIV,

    /* Symbol / punctuation (US layout) */
    APP_KEY_GRAVE,       /* ` ~ */
    APP_KEY_MINUS,       /* - _ */
    APP_KEY_EQUAL,       /* = + */
    APP_KEY_LBRACKET,    /* [ { */
    APP_KEY_RBRACKET,    /* ] } */
    APP_KEY_BACKSLASH,   /* \ | */
    APP_KEY_SEMICOLON,   /* ; : */
    APP_KEY_APOSTROPHE,  /* ' " */
    APP_KEY_COMMA,       /* , < */
    APP_KEY_PERIOD,      /* . > */
    APP_KEY_SLASH,       /* / ? */

    /* System */
    APP_KEY_PAUSE, APP_KEY_PRINT_SCREEN, APP_KEY_MENU,

    APP_KEY_COUNT

} app_key_t;

/* clang-format on */

typedef enum app_mouse_button_e
{
    APP_MOUSE_LEFT   = 0,
    APP_MOUSE_RIGHT  = 1,
    APP_MOUSE_MIDDLE = 2,
    APP_MOUSE_X1     = 3, /* extra button 4 (back)    */
    APP_MOUSE_X2     = 4, /* extra button 5 (forward) */

    APP_MOUSE_BUTTON_COUNT

} app_mouse_button_t;

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct app_api_s
{
    /* ---- Window ---- */

    /* Open a native window. Returns APP_WIN_INVALID on failure.
       x = y = 0 → OS centers; w = h = 0 → 50% of desktop work area. */
    win_id_t ( *window_open )( const char* title, i32 x, i32 y, i32 w, i32 h, u32 flags );

    /* Destroy a window and release its OS resources. */
    void ( *window_close )( win_id_t id );

    bool ( *window_is_valid )( win_id_t id );
    void* ( *window_handle )( win_id_t id ); /* HWND on Windows */
    bool ( *window_is_minimized )( win_id_t id );
    app_win_state_t ( *window_state )( win_id_t id );
    void ( *window_set_fillscreen )( win_id_t id, bool enabled );
    void ( *window_toggle_fillscreen )( win_id_t id );

    /* Default OS background paint/erase. When enabled, Windows fills the client
       area with the registered class brush on WM_ERASEBKGND. Disable once a
       renderer owns the window's pixels — leaving it on causes flicker. */
    void ( *window_set_paint )( win_id_t id, bool enabled );
    void ( *window_toggle_paint )( win_id_t id );
    bool ( *window_paint_enabled )( win_id_t id );

    /* ---- Event loop ---- */

    /* Drain the OS message queue, snapshot input state, fill the event ring buffer.
       Returns false when the application should exit. */
    bool ( *pump_events )( void );

    /* Pull the next typed event from the ring buffer. Returns false when empty. */
    bool ( *next_event )( app_event_t* out );

    bool ( *should_quit )( void );

    /* ---- Input snapshot ---- */

    bool ( *key_down )( app_key_t key );
    bool ( *key_pressed )( app_key_t key );
    bool ( *key_released )( app_key_t key );

    void ( *mouse_position )( i32* out_x, i32* out_y );
    bool ( *mouse_button_down )( app_mouse_button_t btn );
    bool ( *mouse_button_pressed )( app_mouse_button_t btn );
    bool ( *mouse_button_released )( app_mouse_button_t btn );

    /* Enable key-repeat events (text mode). Default false = game mode (repeats suppressed). */
    void ( *key_repeat_set )( bool enabled );

} app_api_t;

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

/*============================================================================================*/
#endif    // APP_H
