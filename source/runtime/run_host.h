#ifndef RUN_HOST_H
#define RUN_HOST_H
/*==============================================================================================

    runtime/run_host.h -- For host executables only.

    Every exe converges on one call:

        int main( int argc, char** argv ) {
            return run_host_main( &k_desc, argc, argv );
        }

    The conventional game loop
    --------------------------
    host.c drives a direct, named-module loop. It knows the engine-level
    modules it manages (app, render) and calls them by name. It does NOT iterate
    the dep graph generically — each module call is intentional.

        [pump OS events] <- app()->pump_events() — false = window closed (when windowed)
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
    app is part of the always-loaded engine floor, so the host cannot infer intent from
    its presence.  A host declares RUN_HOST_WINDOWED to open a window and pump OS events;
    without it the host is headless (server, tool).  If render is loaded it drives the
    render loop; the render services (rhi/draw/gui/render) remain opt-in via k_modules[].
    gui is a fully OPTIONAL service: without it the host runs a plain platform window; with
    it the host wires the service (caps, font, frame hooks, viewport) but keeps ownership of
    the window, the rhi context, the loop, and the pacing.

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
    Windowed:  an unvetoed main-window WIN_CLOSE (on_close_request), or an OS-level quit
               honored after the event drain -- the drain always runs before the quit flag
               is checked, so on_close_request is guaranteed its look; a veto calls
               app()->quit_reset() and the loop carries on.
    Headless:  run_host_quit() sets a flag checked at the top of each frame.
    Both paths lead to the same clean shutdown: mod_system_exit() in reverse dep order.

==============================================================================================*/

#include "orb.h"
#include "runtime/run.h"                /* run_clock_t / run_frame_stats_t          */
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
    RUN_HOST_WINDOWED     = 1 << 4, /* open a window and pump OS events.  app is always
                                       loaded (engine floor), so windowed vs headless is
                                       explicit policy, not an app()-presence inference;
                                       server / tool hosts leave this clear             */
};

/*============================================================================================*/

typedef enum run_loop_mode_e
{
    RUN_LOOP_NONE, /* host inits, then returns with everything live — the caller
                      drives modules at its own call site and MUST call
                      run_host_shutdown() when done (single-shot tool calls)     */
    RUN_LOOP_ONCE, /* one full tick, then exit (tools, asset processors)        */
    RUN_LOOP_RUN,  /* run until run_host_quit() or pump_events() returns false  */

} run_loop_mode_t;

/*==============================================================================================
    Module entries

    k_modules[] declares only the layers ABOVE the engine floor.  run_host_main always loads
    the floor itself -- sys, ref, prof, fs, job, net, app, core, run -- regardless of what a
    host declares; these root engine libraries are cheap and create no OS resources on load
    (job spawns no threads until job_workers configures the pool, net opens no sockets, app
    opens no window).  A host lists only the services with real init cost: rhi, draw, gui,
    render, game.
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
    gui_builtin_font_t        font;       // GUI_FONT_NONE = host loads its own (font_load)
    const gui_forward_caps_t* caps;       // NULL = GUI_FORWARD_CAPS_DEFAULT                 */
    f32                       clear[ 4 ]; // main-surface clear color; alpha 0 = default dark
    bool                      debug;      // arm the gui debug hotkey driver (P/O/F9/F10...)  */

} run_gui_desc_t;

typedef struct run_host_desc_s
{
    const char*               name;               // host name for logging and window title
    u32                       flags;              // RUN_HOST_*
    run_loop_mode_t           loop_mode;          // determines how the main loop is driven
    i32                       frame_target_ms;    // 0 -> default 16
    i32                       job_workers;        // job pool size: 0 (default) = main-thread
                                                  //   only, N = N workers, -1 = auto (per-core)
    i32                       window_width;       // client area width,  0 -> 1280
    i32                       window_height;      // client area height, 0 -> 720
    const run_module_entry_t* modules;            // null-terminated array
    const run_gui_desc_t*     gui;                // optional gui config; NULL = defaults

    /* Optional game project DLL -- Tier-3, always dynamic, loaded with mod_dynamic_load_dir
       AFTER modules[] registers and BEFORE mod_init_all (one dep-ordered init pass covers
       it; its higher-layer deps -- game/render -- must be in modules[]; core is baseline).
       The runtime is contract-
       agnostic: it loads and hot-reloads the DLL but never calls into it.  Drivers fetch the
       vtable via mod_get_api( project_name ) and drive it (see runtime/run_project.h). */

    const char*               project_name;       // project DLL base name; NULL = none
    const char*               project_dir;        // dir holding <name>.dll; NULL = exe dir

    void ( *on_ready )( void );                   // after init + window creation
    void ( *on_update )( f32 dt );                // each frame — game logic, no widgets
    void ( *on_gui )( f32 dt );                   // dirty frames only — the widget build
    bool ( *on_close_request )( void );           // X pressed: true = quit, false = veto
    bool ( *on_event )( const app_event_t* ev );  // unconsumed events; true = consumed

} run_host_desc_t;

/*==============================================================================================
    API
==============================================================================================*/

int run_host_main( const run_host_desc_t* desc, int argc, char** argv );

/* Teardown for RUN_LOOP_NONE hosts: run_host_main returned with everything live, the
   caller did its work, and now releases it all (gui -> draw -> rhi -> window -> mod).
   RUN_LOOP_ONCE / RUN_LOOP_RUN call this internally — do not call it a second time. */
void run_host_shutdown( void );

/* Host-owned handles, valid after run_host_main's init (on_ready onward).  Sentinels
   when the owning service is absent: APP_WIN_INVALID / RHI_CTX_INVALID (-1) /
   GUI_VP_INVALID.  Use these instead of hardcoding context 0 / viewport 0. */
win_id_t run_host_window( void ); /* main platform window          */
i32      run_host_ctx( void );    /* main rhi context id           */
gui_vp_t run_host_vp( void );     /* gui viewport of the main window */

/* headless quit — sets flag, checked each frame top */
void run_host_quit( void );
bool run_host_should_quit( void );

/* editor sleep diagnostics — toggle or set from on_update / on_ready */
void run_host_sleep_debug_set( bool enabled );
void run_host_sleep_debug_toggle( void );

/* Realtime gate: while active, RUN_HOST_EDITOR_SLEEP is suspended and the loop paces
   at frame_target_ms like a game host.  Core to editor simulate/play/stop -- assert it
   while a game session is live (typically re-derived every frame in on_update from the
   session state); clear it when the session stops and idle blocking resumes.  Hosts
   without RUN_HOST_EDITOR_SLEEP may call it freely -- it is a no-op there. */
void run_host_realtime_set( bool active );
bool run_host_realtime( void );

/* called once per frame by the host before on_update; now_us is the integer
   microsecond tick (sys_tick_microseconds).  The clock diffs it against the previous
   stamp internally -- floats are derived here, never accumulated.  Modules must not call. */
void run_clock_update( u64 now_us );

/* called once per frame by the host after pacing with that frame's phase timings;
   published read-only via run()->frame_stats().  Modules must not call. */
void run_clock_stats_submit( const run_frame_stats_t* stats );

/*============================================================================================*/
#endif /* RUN_HOST_H */
