/*==============================================================================================

    mod.h

    Hot-reload module system — public interface.

    Lifecycle
    ---------
        module_system_init()        boot; no modules loaded yet
        mod_static_load( name )     register a static module (no DLL, no shadow copy)
        module_load( name )         register and load a module (static or dynamic, see below)
        module_init_all()           topo-sort deps → call each module's init()
        mod_system_tick( dt )       tick all INITIALIZED modules in dep order
        mod_check_reloads()         poll file timestamps → debounce → reload changed DLLs
        module_system_exit()        exit + unload in reverse dep order; delete shadow files

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

/* Hot-reload a single module in place:
   Call exit → unload DLL → shadow copy → load new DLL → Call on_reload() or init().
   State pointer is preserved; block is NOT zeroed on reload. */
bool mod_reload( const char* name );

/* Force-reload every dynamic module immediately, skipping the file-watch debounce.
   Returns the count of modules successfully reloaded. Useful for an editor's
   "Reload Modules" command. Static modules are skipped. */
int mod_reload_all( void );

/*==============================================================================================
    Initialization and update
==============================================================================================*/

/* Topologically sort dependencies and call init() on every LOADED module.
   Returns false on a dependency cycle or if any init() returns false. */
bool mod_init_all( void );

/* Tick every INITIALIZED module in dependency order. */
void mod_system_tick( float dt );

/* Poll file timestamps and hot-reload any DLL that has changed on disk.
   Applies a debounce window (DEBOUNCE_MS) to avoid reloading while the linker
   is still writing the file. */
void mod_check_reloads( void );

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