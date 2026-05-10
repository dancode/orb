/*==============================================================================================

    engine/mod/mod.h : Hot-reload module system — public interface.

    Lifecycle
    ---------
        mod_system_init()           boot; no modules loaded yet
        mod_static_load( name )     register a static module (no DLL, no shadow copy)
        mod_load( name )            register and load a module (static or dynamic)
        mod_init_all()              topo-sort deps → call each module's init()
        mod_check_reloads()         poll file timestamps → enqueue changed DLLs
        mod_reload( name )          enqueue a single module for reload
        mod_reload_all()            enqueue every dynamic module for reload
        mod_system_flush_reloads()  perform queued swaps; call once per frame at sync point
        mod_system_exit()           exit + unload in reverse dep order; delete shadow files
    
    Reload model
    ------------
    Reloads are NEVER applied synchronously. mod_reload, mod_reload_all, and the
    file watcher all just enqueue. The host calls mod_system_flush_reloads() at
    a single, well-defined point in its main loop — typically the end of the
    frame, after simulation and rendering are complete and no module code is
    in flight. This is the only point at which a DLL swap is provably safe.

    module_load( name ) build variants
    -----------------------------------
    Define BUILD_STATIC for a monolithic build where all modules are linked into the exe.
    Omit it for a dynamic build where each module is a separate DLL.

        BUILD_STATIC defined:
            module_load( render ) → mod_static_load( "render", render_get_mod_api() )

        BUILD_STATIC not defined:
            module_load( render ) → mod_dynamic_load( "render" )
                                    shadow-copies render.dll → resolves exports → allocs state

    Querying APIs from outside the module system (host exe or another module):

        Static build — direct, zero-cost (LTO-devirtualizable):
            render_api()->draw( dt );

        Dynamic build — one pointer load per call:
            render_api()->draw( dt );   // identical call site; MODULE_GATEWAY handles the rest

        See MODULE_GATEWAY, MOD_DEFINE_API_PTR, MOD_FETCH_API in mod_api.h.

==============================================================================================*/
#ifndef MOD_H
#define MOD_H

#include "orb.h"

/*==============================================================================================
    module_load macro
==============================================================================================*/

// mod_static_load( #name, name##_get_mod_api(), ( void* )name##_get_api() )

#ifdef BUILD_STATIC
    #define mod_load( name ) mod_static_load( #name, name##_get_mod_api() )
#else
    #define mod_load( name ) mod_dynamic_load( #name )
#endif

/*==============================================================================================
    Module Registry Entry
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

/* Register a statically-linked module.
   mod_api must have func_api set to the module's exported API struct.
   Does NOT call init() — call module_init_all() once all modules are registered. */
bool mod_static_load( const char* name, mod_api_t* mod_api );    // , void* exported_api

/* Load a DLL by base name (e.g. "render" → "<exe_dir>/render.dll").
   Creates a shadow copy, loads it, resolves exports, allocates state.
   Does NOT call init() — call module_init_all() once all modules are loaded. */
bool mod_dynamic_load( const char* name );

/* Unload a single module: call exit(), free state, unload DLL, delete shadow file. */
bool mod_unload( const char* name );

/* Enqueue a reload to be performed at the next mod_system_flush_reloads() call.
   Returns false only if `name` is not registered. */
bool mod_reload( const char* name );

/* Enqueue every dynamic module for reload. Returns the number queued. */
int mod_reload_all( void );

/* Poll file timestamps and enqueue any DLL whose original file changed.
   Debounce is applied at flush time, not here. */
void mod_check_reloads( void );

/* Drain the pending reload queue. Forced entries (from mod_reload / mod_reload_all)
   reload immediately; file-watch entries wait out the debounce window.
   Returns the number of modules actually reloaded this call.
   Call this once per frame at a quiescent point. */
int mod_system_flush_reloads( void );

/*==============================================================================================
    Initialization and update
==============================================================================================*/

/* Topologically sort dependencies and call init() on every LOADED module.
   Returns false on a dependency cycle or if any init() returns false. */
bool mod_init_all( void );


/*==============================================================================================
    API access
==============================================================================================*/

/* Returns the exported_api pointer for a named, INITIALIZED module, else NULL.
   Call the module specific function render_api(), audio_api() for direct access.
   Cast the result to the module's typed struct:
       core_api_t*   core = MOD_FETCH_API("core");
       render_api_t* rend = MOD_FETCH_API("render");  */
const void* mod_get_api( const char* name );

/* Last error message from any failed operation. */
const char* mod_last_error( void );

/*==============================================================================================
    Debug
==============================================================================================*/

/* Debug helpers */
void mod_list_all( void );

/*============================================================================================*/
#endif    // MOD_H