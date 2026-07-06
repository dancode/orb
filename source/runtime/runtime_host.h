#ifndef RUNTIME_HOST_H
#define RUNTIME_HOST_H
/*==============================================================================================

    runtime/host.h — For host executables only.

    Every exe converges on one call:

        int main( int argc, char** argv ) {
            return run_host_main( &k_desc, argc, argv );
        }

    The conventional game loop
    --------------------------
    host.c drives a direct, named-module loop. It knows the engine-level
    modules it manages (app, render) and calls them by name. It does NOT iterate
    the dep graph generically — each module call is intentional.

        [pump OS events] <- app()->pump_events() — false = window closed (when app is loaded)
        [event drain]    <- rhi()->event / gui()->event routing; on_event sees the rest;
                            WIN_CLOSE consults on_close_request before quitting
        [console poll]   <- sys, if RUN_HOST_CONSOLE
        [clock update]   <- run_clock_update() — stamps app_time, dt, frame_number
        [on_update]      <- desc callback — game logic, every frame, no widget calls
        [gui emit]       <- gated on gui()->frame_begin's dirty bool: chrome shell (when
                            borderless) then on_gui — the ONLY place to emit widgets
        [render]         <- render() frame when loaded; else host-driven gui composite
        [hot-reload]     <- mod_check_reloads + flush, if RUN_HOST_HOT_RELOAD

    Windowed vs headless
    --------------------
    The host infers its mode from k_modules[]: if app is loaded, the host creates a
    window and pumps OS events. If render is also loaded, it drives the render loop.
    No separate flag — k_modules[] is the single declaration of intent.  gui is a fully
    OPTIONAL service: without it the host runs a plain platform window; with it the host
    wires the service (caps, font, frame hooks, viewport) but keeps ownership of the
    window, the rhi context, the loop, and the pacing.

    Callbacks
    ---------
    on_ready         : called once after mod_init_all() and window creation (if windowed).
                       Use for one-time setup — render() and app() are live here.
    on_update        : called every frame, after clock update, before the gui emit.
                       Game logic only — widget calls belong in on_gui.  Receives f32 dt
                       (capped, time-scaled); run()->clock() has richer timing.
    on_gui           : UI emission; called only when gui is loaded AND the frame is dirty
                       (gui()->frame_begin returned true — clean retained frames skip it).
                       Runs inside the default context's build, after the chrome shell.
    on_close_request : main-window X pressed; return true to allow the quit, false to veto
                       ("unsaved changes" flows). NULL = close immediately.  run_host_quit()
                       is programmatic and final — it does not consult this.
    on_event         : optional raw-event tap; sees each event rhi/gui did not consume;
                       return true to consume it (a consumed WIN_CLOSE skips the quit path).

    Quit
    ----
    Windowed:  app()->pump_events() returning false, or an unvetoed main-window WIN_CLOSE.
    Headless:  run_host_quit() sets a flag checked at the top of each frame.
    Both paths lead to the same clean shutdown: mod_system_exit() in reverse dep order.

==============================================================================================*/

#include "orb.h"
#include "engine/app/app.h"             /* app_event_t (types only)                 */
#include "runtime_service/gui/gui.h"    /* gui font / caps types (types only — no
                                           link dependency; gui remains optional)   */

/*============================================================================================*/

enum    // RUN_HOST_FLAGS
{
    RUN_HOST_HOT_RELOAD   = 1 << 0, /* poll DLL changes + flush each frame        */
    RUN_HOST_CONSOLE      = 1 << 1, /* sys_console_input_init / poll / shutdown    */
    RUN_HOST_EDITOR_SLEEP = 1 << 2, /* block on OS input when idle instead of
                                       spinning at frame_target_ms; use for tools
                                       and editors, not game loops               */
    RUN_HOST_BORDERLESS   = 1 << 3, /* borderless main window with gui-drawn chrome;
                                       honored only when gui is in k_modules[] (gui
                                       draws the shell) — plain frame otherwise    */
};

/*============================================================================================*/

typedef enum run_loop_mode_e
{
    RUN_LOOP_NONE, /* host inits, then returns — caller drives ticks manually   */
    RUN_LOOP_ONCE, /* one full tick, then exit (tools, asset processors)        */
    RUN_LOOP_RUN,  /* run until run_host_quit() or pump_events() returns false  */

} run_loop_mode_t;

/*==============================================================================================
    Module entries
==============================================================================================*/

typedef struct mod_desc_s mod_desc_t;
typedef mod_desc_t* ( *run_get_mod_desc_fn )( void );

typedef struct
{
    const char*        name;
    run_get_mod_desc_fn get_mod_desc; /* NULL -> load as DLL */

} run_module_entry_t;

#define RUN_SERVICE( n ) { #n, n##_get_mod_desc }

#ifdef BUILD_STATIC
    #define RUN_MODULE( n ) { #n, n##_get_mod_desc }
#else
    #define RUN_MODULE( n ) { #n, NULL }
#endif

/*==============================================================================================
    Host descriptor
==============================================================================================*/

/* Optional gui service configuration — pointed to from the host descriptor; NULL keeps
   today's defaults (no font, default caps, dark clear, debug driver off).  Only read when
   gui is in k_modules[]. */
typedef struct
{
    gui_builtin_font_t        font;       /* GUI_FONT_NONE = host loads its own (font_load)  */
    const gui_forward_caps_t* caps;       /* NULL = GUI_FORWARD_CAPS_DEFAULT                 */
    f32                       clear[ 4 ]; /* main-surface clear color; alpha 0 = default dark */
    bool                      debug;      /* arm the gui debug hotkey driver (P/O/F9/F10...)  */

} run_gui_desc_t;

typedef struct run_host_desc_s
{
    const char*               name;               /* host name for logging and window title  */
    u32                       flags;              /* RUN_HOST_*                              */
    run_loop_mode_t           loop_mode;          /* determines how the main loop is driven  */
    i32                       frame_target_ms;    /* 0 -> default 16                         */
    i32                       window_width;       /* client area width,  0 -> 1280           */
    i32                       window_height;      /* client area height, 0 -> 720            */
    const run_module_entry_t* modules;            /* null-terminated array                   */
    const run_gui_desc_t*     gui;                /* optional gui config; NULL = defaults    */
    void ( *on_ready )( void );                   /* after init + window creation            */
    void ( *on_update )( f32 dt );                /* each frame — game logic, no widgets     */
    void ( *on_gui )( f32 dt );                   /* dirty frames only — the widget build    */
    bool ( *on_close_request )( void );           /* X pressed: true = quit, false = veto    */
    bool ( *on_event )( const app_event_t* ev );  /* unconsumed events; true = consumed      */

} run_host_desc_t;

/*==============================================================================================
    API
==============================================================================================*/

int run_host_main( const run_host_desc_t* desc, int argc, char** argv );

/* headless quit — sets flag, checked each frame top */
void run_host_quit( void );
bool run_host_should_quit( void );

/* editor sleep diagnostics — toggle or set from on_update / on_ready */
void run_host_sleep_debug_set( bool enabled );
void run_host_sleep_debug_toggle( void );

/* called once per frame by the host before on_update. Modules must not call. */
void run_clock_update( f64 app_time, f32 dt_real );

/*============================================================================================*/
#endif /* RUNTIME_HOST_H */
