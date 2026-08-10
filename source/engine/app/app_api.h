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
    /*=======================================================================================*/
    /* ---- Window ---- */

    /* Open a native window. Returns APP_WIN_INVALID on failure.
       x = y = 0 -> OS centers; w = h = 0 -> 50% of desktop work area. */
    win_id_t ( *window_open )( const char* title, i32 x, i32 y, i32 w, i32 h, u32 flags );

    /* Destroy a window and release its OS resources. */
    void ( *window_close )( win_id_t id );

    bool            ( *window_is_valid          )( win_id_t id );
    void*           ( *window_handle            )( win_id_t id ); /* HWND on Windows */
    bool            ( *window_is_minimized      )( win_id_t id );

    /* True when the window was opened with APP_WIN_BORDERLESS (custom frame: no OS chrome, gui
       stands in for the caption / sizing border).  Lets a chrome-drawing layer decide whether a
       frame shell is needed without the host threading the flag through.  false on invalid id. */
    bool            ( *window_is_borderless     )( win_id_t id );

    /* Client-area drawable size in pixels.  Always reflects the current size; returns 0,0
       for an invalid id.  Use this in context_open / viewport_open to avoid passing w/h
       explicitly when the window already owns those dimensions. */
    void            ( *window_get_size          )( win_id_t id, i32* out_w, i32* out_h );

    /* Screen-space geometry.  window_get_pos returns the window's CLIENT-area top-left in
       virtual-desktop screen coordinates; window_set_pos moves the window so its CLIENT corner
       lands at the given screen point (frame offset handled internally).  Paired with
       mouse_position_screen (below) these let a multi-window UI place / track a torn-off OS window
       at an exact screen location, independent of which window the cursor is over. */
    void            ( *window_get_pos           )( win_id_t id, i32* out_x, i32* out_y );
    void            ( *window_set_pos           )( win_id_t id, i32 x, i32 y );

    /* The window's current monitor scale factor (1.0 = 96 DPI, 1.5 = 150%, ...).  The process
       declares Per-Monitor V2 awareness at first window open, so all sizes/coords everywhere in
       the engine are PHYSICAL pixels; this factor is advisory -- a UI layer multiplies its
       metrics by it to keep apparent size, or ignores it to stay 1:1.  Updates arrive as
       APP_EV_WIN_DPI events (queued before the resize they cause).  1.0 on an invalid id and on
       systems without per-monitor DPI support. */
    f32             ( *window_dpi_scale         )( win_id_t id );

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

    /* ---- Native-borderless window actions (window kind 3) ----

       A borderless window has no Win32 non-client area, so the gui titlebar /
       borders stand in for it.  These hand a grab gesture back to the OS, which
       runs its own modal move / resize loop (the fiber message pump keeps the
       game loop rendering throughout).  All are no-ops on an invalid id. */

    /* Begin a native move: gui calls this when the cursor grabs the titlebar.
       Drag-to-screen-edge Aero Snap and dragging follow for free. */
    void ( *window_start_move )( win_id_t id );

    /* Begin a native resize from the given border / corner zone. */
    void ( *window_start_resize )( win_id_t id, app_win_zone_t zone );

    /* Double-click-titlebar gesture: native maximize / restore toggle. */
    void ( *window_title_event )( win_id_t id );

    /* Show the native system menu at client-space (x,y) -- e.g. right-click on
       the titlebar.  Leaves fillscreen first, then dispatches the chosen command. */
    void ( *window_system_menu )( win_id_t id, i32 x, i32 y );

    /* Publish the edge-resize grab thickness for a native-borderless window.  gui calls this each
       frame for an GUI_WIN_NATIVE window.  border is the edge-grab thickness in client px (<= 0
       disables resize).  gui now owns the entire client surface (HTCLIENT everywhere inside the
       border band) and dispatches move / title / system-menu gestures through window_start_move,
       window_title_event, and window_system_menu rather than routing them through HTCAPTION. */
    void ( *window_set_native_frame )( win_id_t id, bool enabled, i32 border );

    /* Publish the live-resize client-area quantum (px) for this window.  During an interactive edge
       drag (WM_SIZING) the OS-proposed client size is snapped to a whole multiple of step_w/step_h,
       so the drawable surface always holds an integer number of gui grid cells and the windows
       snapped inside (which rest on the same lattice) align flush with its edges.  gui republishes
       this each frame from grid_quantum; a step <= 1 disables snapping (free-pixel resize). */
    void ( *window_set_size_step )( win_id_t id, i32 step_w, i32 step_h );

    /* Request a graceful close: post WM_CLOSE so the normal close path runs (main window quits,
       an gui-owned floater is torn down).  Unlike window_close it does not destroy immediately. */
    void ( *window_request_close )( win_id_t id );

    /* Add / remove the native sizing frame (controls whether border resize works). */
    void ( *window_enable_resize )( win_id_t id, bool enabled );

    /* Set the OS mouse cursor for this window. */
    void ( *window_set_cursor )( win_id_t id, app_cursor_t cursor );

    /* Maximize / toggle.  Mirror the existing window_minimize / window_restore;
       the min / max state is observable through window_state(). */
    void ( *window_maximize        )( win_id_t id );
    void ( *window_toggle_maximize )( win_id_t id );

    /*=======================================================================================*/
    /* ---- Event loop ---- */

    // Drain the OS message queue, snapshot input state, fill the event ring buffer.
    // Returns false when the application should exit.
    bool ( *pump_events )( void );

    // Pull the next typed event from the ring buffer. Returns false when empty.
    // Call pump first to fill the ring buffer, then call next_event repeatedly until false.
    bool ( *next_event  )( app_event_t* out );

    // True if the app should exit (quit flag set by WM_CLOSE or app()->quit_request()).
    bool ( *should_quit )( void );

    // Cancel a pending quit. The main window's WM_CLOSE arms the quit flag AND queues 
    // APP_EV_WIN_CLOSE in the same pump; 
    // A host that vetoes the close (run_host.h -- on_close_request cb returns false) 
    // calls this after draining the event so pump_events resumes returning true.
    void ( *quit_reset )( void );

    /*=======================================================================================*/
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

    /* ---- Raw mouse / relative mode ---- */

    /* Relative mouse mode for FPS-style look: hides the cursor and confines it to `id`'s
       client area; read motion via mouse_raw_delta (the window's MOUSE_MOVE events are
       suppressed while enabled -- buttons and wheel still flow).  Disabling restores the
       pointer where it was at enable.  The clip releases on focus loss and re-applies on
       refocus automatically. */
    void ( *mouse_relative_set )( win_id_t id, bool enabled );
    bool ( *mouse_is_relative  )( win_id_t id );

    /* This frame's accumulated raw hardware deltas (WM_INPUT) in device counts -- no OS
       pointer ballistics, no screen-edge clamping.  Valid every frame regardless of
       relative mode; zero when the mouse did not move. */
    void ( *mouse_raw_delta )( f32* out_dx, f32* out_dy );

    /* ---- Gamepad (XInput on Windows) ---- */

    /* Pad BUTTONS need no dedicated queries: edges post APP_EV_KEY_DOWN/UP with APP_SRC_PAD_*
       codes and set the same snapshot arrays as keyboard keys, so key_down( APP_SRC_PAD_A )
       etc. answer directly (multiple pads OR together until player assignment lands in the
       input service).  These entries cover what buttons cannot: presence, analog, rumble. */

    bool ( *pad_connected )( i32 pad ); /* pad 0..APP_PAD_MAX-1 */

    /* Raw normalized axis: sticks -1..1 (+right / +up), triggers 0..1.  No deadzone, no
       curve -- filtering is input-service policy.  0 for invalid / disconnected pads. */
    f32 ( *pad_axis )( i32 pad, app_pad_axis_t axis );

    /* Motor speeds 0..1 (lo = left/low-frequency, hi = right/high-frequency).  Latches
       until changed -- pass 0,0 to stop. */
    void ( *pad_rumble )( i32 pad, f32 lo, f32 hi );

    /* ---- Source names ---- */

    /* The unified source-space name table ("w", "mouse1", "pad_a"), indexed by app_src_t
       code; NULL at gap indexes.  Static data, valid for the process lifetime.  Lets a
       service resolve config-file source names without host glue (the host separately
       wires the same table into the core bind system). */
    const char* const* ( *key_names )( u32* out_count );

    /* ---- Clipboard ---- */

    /* Copy NUL-terminated `text` to the OS clipboard (the outbound half: cut / copy).
       The inbound half (paste) is delivered as an APP_EV_CLIPBOARD event when the user
       presses the paste gesture, so reading the clipboard needs no polling API. */
    void ( *clipboard_set )( const char* text );

} app_api_t;

/*============================================================================================*/

/* app is part of the always-loaded engine floor (statically linked into every host, loaded
   unconditionally in run_host_main), so it uses the hard-bound static gateway -- app() is never
   NULL.  "Windowed" is no longer inferred from app()'s presence; it is explicit host policy
   (RUN_HOST_WINDOWED).  Unlike rhi/draw/gui/render this ignores MOD_HOST_DYNAMIC_SERVICES. */
#if defined( BUILD_STATIC ) || defined( APP_STATIC )
    MOD_GATEWAY_STATIC( app_api_t, app )
    #define MOD_USE_APP    /* static build */
    #define MOD_FETCH_APP  true
#else
    MOD_GATEWAY_DYNAMIC( app_api_t, app )
    #define MOD_USE_APP    MOD_DEFINE_API_PTR( app_api_t, app )
    #define MOD_FETCH_APP  MOD_FETCH_API( app_api_t, app )
#endif

// clang-format on
/*============================================================================================*/
#endif    // APP_API_H
