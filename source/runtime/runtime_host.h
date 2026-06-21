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
        [console poll]   <- sys, if RUN_HOST_CONSOLE
        [clock update]   <- run_clock_update() — stamps app_time, dt, frame_number
        [on_update]      <- desc callback — host-level logic, game bootstrap
        [render]         <- begin_frame / draw_frame / end_frame (when render is loaded)
        [hot-reload]     <- mod_check_reloads + flush, if RUN_HOST_HOT_RELOAD

    Windowed vs headless
    --------------------
    The host infers its mode from k_modules[]: if app is loaded, the host creates a
    window and pumps OS events. If render is also loaded, it drives the render loop.
    No separate flag — k_modules[] is the single declaration of intent.

    Callbacks
    ---------
    on_ready  : called once after mod_init_all() and window creation (if windowed).
                Use for one-time setup — render() and app() are live here.
    on_update : called every frame, after clock update, before render.
                Receives f32 dt (capped, time-scaled). Call run()->clock() for
                richer timing (app_time, frame_number, time_scale, dt_real).

    Quit
    ----
    Windowed:  app()->pump_events() returning false breaks the loop.
    Headless:  run_host_quit() sets a flag checked at the top of each frame.
    Both paths lead to the same clean shutdown: mod_system_exit() in reverse dep order.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

enum    // RUN_HOST_FLAGS
{
    RUN_HOST_HOT_RELOAD   = 1 << 0, /* poll DLL changes + flush each frame        */
    RUN_HOST_CONSOLE      = 1 << 1, /* sys_console_input_init / poll / shutdown    */
    RUN_HOST_EDITOR_SLEEP = 1 << 2, /* block on OS input when idle instead of
                                       spinning at frame_target_ms; use for tools
                                       and editors, not game loops               */
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

typedef struct run_host_desc_s
{
    const char*               name;               /* host name for logging and window title  */
    u32                       flags;              /* RUN_HOST_*                              */
    run_loop_mode_t           loop_mode;          /* determines how the main loop is driven  */
    i32                       frame_target_ms;    /* 0 -> default 16                         */
    i32                       window_width;       /* client area width,  0 -> 1280           */
    i32                       window_height;      /* client area height, 0 -> 720            */
    const run_module_entry_t* modules;            /* null-terminated array                   */
    void ( *on_ready )( void );                   /* after init + window creation            */
    void ( *on_update )( f32 dt );                /* each frame, between clock and render    */

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
