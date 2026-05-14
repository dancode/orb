/*==============================================================================================

    runtime/app/runtime_app.h — Windowed application runtime, public interface.

    A thin wrapper above runtime_host. Handles the orchestration every windowed
    host shares — window create/destroy, per-frame event pumping — so the host's
    callbacks contain only application logic.

    Layering
    --------
        sys + mod                       (substrate, always linked)
            |
        runtime_host                    (generic loop orchestrator)
            |
        runtime_app  <-- this file      (window + event pump)
            |
        host executable                 (game logic only)

    Build-mode flags (load_core, load_app, etc.) are NOT exposed on the config.
    Loading core and app is implicit — they are why this layer exists. A host
    that wants to opt out of either should drop down to runtime_host directly.

    Lifecycle inside runtime_app_run()
    -----------------------------------
        1. Build a runtime_config_t with our own wrapper callbacks.
        2. Call runtime_host_run().
            - sys / core / app / modules load and init as usual.
            - Our wrapper on_init runs: window_create(), then user on_init.
            - Our wrapper on_update runs each frame: pump_events(), then user
              on_update. Returning false from either exits the loop.
            - Our wrapper on_exit runs: user on_exit first (window still
              exists), then window_destroy().
        3. runtime_host shuts the module system down.

==============================================================================================*/
#ifndef RUNTIME_APP_H
#define RUNTIME_APP_H

#include "orb.h"
#include "runtime/host/runtime_host.h"

/*==============================================================================================
    Defaults
==============================================================================================*/

#define RUNTIME_APP_DEFAULT_WIDTH  1280
#define RUNTIME_APP_DEFAULT_HEIGHT 720
#define RUNTIME_APP_DEFAULT_TITLE  "Orb"

/*==============================================================================================
    Window Config
==============================================================================================*/

typedef struct rt_app_window_s
{
    const char* title;  /* UTF-8 title bar text. NULL → RUNTIME_APP_DEFAULT_TITLE */
    i32         width;  /* client-area width.  0 → RUNTIME_APP_DEFAULT_WIDTH      */
    i32         height; /* client-area height. 0 → RUNTIME_APP_DEFAULT_HEIGHT     */

} rt_app_window_t;

/*==============================================================================================
    Config
==============================================================================================*/

typedef struct rt_app_config_s
{
    const char*              host_name;
    rt_app_window_t          window;
    const rt_module_entry_t* modules;

    runtime_init_fn          on_init;
    runtime_update_fn        on_update;
    runtime_exit_fn          on_exit;
    void*                    userdata;

    i32                      frame_target_ms;

    bool                     load_rhi; /* rhi is still opt-in */
    bool                     enable_hot_reload;

} rt_app_config_t;

/*==============================================================================================
    API
==============================================================================================*/

/* Run the windowed host. Blocks until the user closes the window, the user's
   on_update returns false, or setup fails. Returns true on clean exit. */

bool runtime_app_run( const rt_app_config_t* config );

/*============================================================================================*/
#endif