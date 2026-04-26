#ifndef MODULE_SYS_H
#define MODULE_SYS_H
/*==============================================================================================

    module_sys.h

    Hot-reload module system — public interface.

    Lifecycle
    ---------
        module_system_init()          boot; no modules loaded yet
        module_register_static()      register a compile-time linked module (no DLL)
        module_dynamic_load()         shadow-copy DLL → resolve API → alloc state
        module_init_all()             topo-sort deps → call each module's init()
        module_system_tick()          tick all INITIALIZED modules in dep order
        module_check_reloads()        poll file timestamps → reload changed DLLs
        module_system_exit()          exit + unload in reverse dep order; delete shadows

    Querying APIs from outside the module system (e.g. from the exe):
        core_api_t*   core = module_get_api("core");
        render_api_t* rend = module_get_api("render");

    Inside a DLL, use the module_get_api_t* passed into init() instead of
    calling module_get_api() directly (the DLL cannot link against the exe).

    Our static modules
    ---------

    core_api.h

    - The core api exports common systems used by all modules and are always available.
    - These include logging, memory allocation, cvar access, file access etc.
    - These do not directly interact with the game execution.

    engine_api.h

    - The engine api is not used to compose functionality but rather to access engine systems.
    - Such as frame time, rendering, input, audio, etc. (the game execution system).

    platform_api.h

    - The platform api is used to access platform specific functionality like file IO,
    - The engine systems abtracted from platform should be preferred when possible.
    - Most modules will use the engine and core APIs, but not all need platform APIs.

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Module Registry Entry
==============================================================================================*/

typedef struct module_api_s module_api_t;

typedef enum module_status_t
{
    MODULE_STATUS_EMPTY       = 0,    // slot unused
    MODULE_STATUS_LOADED      = 1,    // DLL loaded, API resolved, state allocated
    MODULE_STATUS_INITIALIZED = 2,    // init() returned true
    MODULE_STATUS_ERROR       = 3,    // load or init failed

} module_status_t;

// Note: sid_t is 32 but unsigned interned string id type (a string offset).

#define MODULE_NAME_MAX 16

typedef struct module_info_s
{
    char            name[ MODULE_NAME_MAX ];    // module name
    module_status_t status;                     // current status of the module

    bool            is_static;       // true → no DLL, no shadow copies
    uint32_t        shadow_count;    // fresh file name generator count to avoid file locking.

    void*           dll;           // handle to the loaded shadow copy
    uint64_t        last_write;    // file-time at last successful load (for change detection)

    module_api_t*   module_api;      // lifecycle api: init/tick/exit/on_reload (called by module system)
    void*           exported_api;    // the module's typed API struct (render_api_t*, etc.)

    void*           state;         // pointer to module-persistent memory struct
    int32_t         state_size;    // size of the allocation; tracked here so we can realloc safely
    int32_t         version;       // currently loaded module version (on reload we bump this)

} module_info_t;

/*==============================================================================================
    System lifetime
==============================================================================================*/

/* Boot the module system. No modules are loaded or initialized yet. */
void module_system_init();

/* Exit and unload every module in reverse dependency order, then clean up. */
void module_system_exit( void );

/*==============================================================================================
    Registration and loading
==============================================================================================*/

/* Register a statically-linked module (no DLL).
   module_api — lifecycle struct (init/tick/exit).
   exported_api — the typed API struct callers receive via get_api("name"). */
bool module_register_static( const char* name, module_api_t* module_api, void* exported_api );

/* Load a DLL by base name (e.g. "render" → "<exe_dir>\render.dll").
   Creates a shadow copy, loads it, resolves both exports, allocates state.
   Does NOT call init() — call module_init_all() once all modules are loaded. */
bool module_dynamic_load( const char* name );

/* Call exit(), unload the DLL, free state, delete the shadow file. */
bool module_unload( const char* name );

/* Hot-reload a single module in place:
   exit → unload DLL → shadow copy → load → init() (state pointer is preserved). */
bool module_reload( const char* name );

/*==============================================================================================
    Initialization and update
==============================================================================================*/

/* Topologically sort dependencies and call init() on every LOADED module.
   Returns false on a dependency cycle or if any init() returns false. */
bool module_init_all( void );

/* Tick every INITIALIZED module in dependency order. */
void module_system_tick( float dt );

/* Poll file timestamps and hot-reload any DLL that has changed on disk. */
void module_check_reloads( void );

/*==============================================================================================
    API access
==============================================================================================*/

/* Returns the exported_api pointer for a named, INITIALIZED module, else NULL.
   Cast the result to the module's typed struct:
       core_api_t*   core = module_get_api("core");
       render_api_t* rend = module_get_api("render");  */
void* module_get_api( const char* name );

/* Last error message from any failed operation. */
const char* module_last_error( void );

/*==============================================================================================
    Debug
==============================================================================================*/

/* Debug helpers */
void module_list_all( void );

/*============================================================================================*/
#endif    // MODULE_SYS_H
