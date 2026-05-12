/*==============================================================================================

    runtime/host/runtime_host.h - Runtime host orchestrator, public interface.

    Owns the full module system lifecycle and the main loop. A host executable
    fills in a runtime_config_t and calls runtime_host_run() — the rest is handled.

    Sequence inside runtime_host_run()
    -----------------------------------
        1.  mod_system_init()
        2.  mod_static_load( "sys", ... )               always
        3.  mod_static_load( "core", ... )              if config->load_core
        4.  Load each entry in config->modules          static or dynamic per build mode
        5.  mod_init_all()
        6.  config->on_init( userdata )                 return false to abort
        7.  Prime the frame timer
        8.  while on_update( dt, userdata ) == true
              mod_check_reloads()                       if enable_hot_reload
              mod_system_flush_reloads()                if enable_hot_reload
              sleep remainder of frame
              tick dt for next frame
        9.  config->on_exit( userdata )
       10.  mod_system_exit()

    Module entry arrays
    -------------------
    Use the RUNTIME_MODULE / RUNTIME_MODULE_END macros to build module lists that
    compile correctly in both static and dynamic builds:

        static const runtime_module_entry_t k_modules[] = {
            RUNTIME_MODULE( render ),
            RUNTIME_MODULE( audio ),
            RUNTIME_MODULE_END
        };

    In BUILD_STATIC this expands each entry to { "render", render_get_mod_api }, with
    the function pointer resolved at link time.  In dynamic builds it expands to
    { "render", NULL } and runtime_host_run calls mod_dynamic_load() instead.

    The module's own API header must be included at the call site for the
    RUNTIME_MODULE macro to compile in BUILD_STATIC mode (it needs name##_get_mod_api).

==============================================================================================*/
#ifndef RUNTIME_HOST_H
#define RUNTIME_HOST_H

#include "orb.h"

/*==============================================================================================
    Module Entry
==============================================================================================*/

/* Forward declaration — full definition lives in mod_export.h.
   Avoids pulling the implementation header into every host. */

typedef struct mod_api_s mod_api_t;
typedef mod_api_t* ( *runtime_get_mod_api_fn )( void );

typedef struct runtime_module_entry_s
{
    const char*            name;    /* module base name, e.g. "render" */
    runtime_get_mod_api_fn get_api; /* non-NULL only in BUILD_STATIC; set by RUNTIME_MODULE */

} rt_module_entry_t;

/* Build-mode-transparent entry constructor.
   In static builds the compiler resolves name##_get_mod_api at link time.
   In dynamic builds the field is NULL and runtime_host_run calls mod_dynamic_load. */

#ifdef BUILD_STATIC
    #define RUNTIME_MODULE( name ) { #name, name##_get_mod_api }
#else
    #define RUNTIME_MODULE( name ) { #name, NULL }
#endif

#define RUNTIME_MODULE_END { NULL, NULL }

/*==============================================================================================
    Callbacks
==============================================================================================*/

/* Called once after mod_init_all() succeeds.
   Return false to abort startup — runtime_host_run will skip the loop and return false. */
typedef bool ( *runtime_init_fn )( void* userdata );

/* Called once per frame.
   dt is the measured wall time of the previous frame in seconds.
   Return false to exit the loop cleanly. */
typedef bool ( *runtime_update_fn )( float dt, void* userdata );

/* Called after the loop exits, before mod_system_exit(). Always called if on_init succeeded. */
typedef void ( *runtime_exit_fn )( void* userdata );

/*==============================================================================================
    Config
==============================================================================================*/

#define RUNTIME_DEFAULT_FRAME_MS 16 /* ~60 FPS target; 0 means use this default */

typedef struct rt_config_s
{
    const char*              host_name; /* printed in startup/shutdown logs */
    const rt_module_entry_t* modules;   /* NULL-terminated array, or NULL for no modules */

    runtime_init_fn          on_init;   /* may be NULL */
    runtime_update_fn        on_update; /* required - provides the quit signal */
    runtime_exit_fn          on_exit;   /* may be NULL */
    void*                    userdata;  /* passed to callbacks, e.g. for state management */

    i32                      frame_target_ms;   /* 0 = RUNTIME_DEFAULT_FRAME_MS (16) */
    bool                     load_core;         /* register engine_core as a static module */
    bool                     enable_hot_reload; /* call mod_check_reloads each frame */

} rt_config_t;

/*==============================================================================================
    API
==============================================================================================*/

/*  Main entry point for the runtime host, call to run the host. 
    Blocks until on_update returns false or setup fails.
    Returns true on a clean exit, false if any setup step failed. */

bool runtime_host_run( const rt_config_t* config );

/*============================================================================================*/
#endif    // RUNTIME_HOST_H