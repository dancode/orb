/*==============================================================================================

    engine/mod/mod.h : Hot-reload module system, public interface.

    Lifecycle
    ---------
        mod_system_init()           Boot; no modules loaded yet.
        mod_static_load( name )     Register a statically-linked module (no DLL).
        mod_load( name )            Register and load a module (expands per build mode).
        mod_init_all()              Topo-sort deps, call each module's init().
        mod_check_reloads()         Poll file timestamps; enqueue changed DLLs.
        mod_reload( name )          Force-queue a single module for reload.
        mod_reload_all()            Force-queue every dynamic module for reload.
        mod_system_flush_reloads()  Apply queued swaps; call once per frame at a safe point.
        mod_system_exit()           Exit in reverse dep order; unload DLLs; delete shadows.
    
    Deferred reload model
    ---------------------
    mod_reload, mod_reload_all, and the file-watcher all enqueue — they never swap
    immediately. The host calls mod_system_flush_reloads() at a single, well-defined
    point each frame (after simulation and rendering, when no module code is in flight).
    That is the only point where a DLL swap is provably safe.

    mod_load build variants
    -----------------------
        BUILD_STATIC defined:
            mod_load( render ) → mod_static_load( "render", render_get_mod_api() )

        BUILD_STATIC not defined:
            mod_load( render ) → mod_dynamic_load( "render" )
                                  shadow-copy → load → resolve exports → alloc state
    
    API access
    ----------
    Call sites are identical in both build modes:

        Static:  render_api()->begin_frame();   // zero-cost; LTO-devirtualizable
        Dynamic: render_api()->begin_frame();   // one pointer load

    See MOD_GATEWAY_STATIC / MOD_GATEWAY_DYNAMIC in mod_api.h.

==============================================================================================*/
#ifndef MOD_H
#define MOD_H

#include "orb.h"

/*==============================================================================================
    Build-mode–transparent module registration macro
==============================================================================================*/

#ifdef BUILD_STATIC
    #define mod_load( name ) mod_static_load( #name, name##_get_mod_api() )
#else
    #define mod_load( name ) mod_dynamic_load( #name )
#endif

/*==============================================================================================
    Types
==============================================================================================*/

typedef struct mod_api_s mod_api_t;

typedef enum module_status_t
{
    MODULE_STATUS_EMPTY       = 0,    // slot unused
    MODULE_STATUS_LOADED      = 1,    // DLL loaded, API resolved, state allocated
    MODULE_STATUS_INITIALIZED = 2,    // init() returned true
    MODULE_STATUS_ERROR       = 3,    // load or init failed

} module_status_t;

/*==============================================================================================
    System lifetime
==============================================================================================*/

/* Boot the module system. No modules are loaded or initialized yet. */
void mod_system_init();

/* Exit every INITIALIZED module in reverse dependency order, then unload DLLs and clean up. */
void mod_system_exit( void );

/*==============================================================================================
    Registration and loading
==============================================================================================*/

/* Register a statically-linked module. Does NOT call init() — use mod_init_all(). */
bool mod_static_load( const char* name, mod_api_t* mod_api );

/* Shadow-copy, load, and register a DLL by base name (e.g. "render" → "<exe_dir>/render.dll").
   Does NOT call init() — use mod_init_all(). */
bool mod_dynamic_load( const char* name );

/* Exit, unload, and deregister a single module. */
bool mod_unload( const char* name );

/* Force-queue a module for reload at the next mod_system_flush_reloads().
   Static modules are silently ignored (returns true). */
bool mod_reload( const char* name );

/* Force-queue every dynamic module for reload. Returns the number queued. */
int mod_reload_all( void );

/* Poll file timestamps and enqueue any DLL whose source file changed.
   Debounce is applied at flush time. */
void mod_check_reloads( void );

/* Drain the pending reload queue. Forced entries reload immediately; file-watch entries
   wait out the debounce window. Returns the number of modules actually reloaded. */
int mod_system_flush_reloads( void );

/*==============================================================================================
    Initialization
==============================================================================================*/

/* Topologically sort by dependency order and call init() on every LOADED module.
   Returns false on a dependency cycle or if any init() returns false. */
bool mod_init_all( void );

/*==============================================================================================
    API access
==============================================================================================*/

/* Returns the stable API pointer for a named, INITIALIZED module, or NULL.
   Prefer the typed accessor (render_api(), audio_api(), etc.) at call sites. */
const void* mod_get_api( const char* name );

/* Human-readable description of the last failed operation. */
const char* mod_last_error( void );

/*==============================================================================================
    Debug
==============================================================================================*/

void mod_list_all( void );

/*============================================================================================*/
#endif    // MOD_H