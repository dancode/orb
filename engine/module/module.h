#ifndef MODULE_SYS_H
#define MODULE_SYS_H
/*==============================================================================================

    module_sys.h

    Hot-reload module system — public interface.

    Lifecycle
    ---------
        module_system_init()        boot; no modules loaded yet
        module_load( name )         register and load a module (static or dynamic, see below)
        module_init_all()           topo-sort deps → call each module's init()
        module_system_tick( dt )    tick all INITIALIZED modules in dep order
        module_check_reloads()      poll file timestamps → debounce → reload changed DLLs
        module_system_exit()        exit + unload in reverse dep order; delete shadow files

    module_load( name ) build variants
    -----------------------------------
    Define BUILD_STATIC for a monolithic build where all modules are linked into the exe.
    Omit it for a dynamic build where each module is a separate DLL.

        BUILD_STATIC defined:
            module_load( render ) → module_static_load( "render", render_get_module_api() )

        BUILD_STATIC not defined:
            module_load( render ) → module_dynamic_load( "render" )
                                    shadow-copies render.dll → resolves exports → allocs state

    Querying APIs from outside the module system (host exe or another module):

        Static build — direct, zero-cost (LTO-devirtualizable):
            render_api()->draw( dt );

        Dynamic build — one pointer load per call:
            render_api()->draw( dt );   // identical call site; MODULE_GATEWAY handles the rest

        See MODULE_GATEWAY, MODULE_DEFINE_API_PTR, MODULE_FETCH_API in module_api.h.

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    module_load macro
==============================================================================================*/

// module_static_load( #name, name##_get_module_api(), ( void* )name##_get_api() )

#ifdef BUILD_STATIC

#    define module_load( name ) \
        module_static_load( #name, name##_get_module_api() )
#else

#    define module_load( name ) module_dynamic_load( #name )

#endif

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

/*==============================================================================================
    System lifetime
==============================================================================================*/

/* Boot the module system. No modules are loaded or initialized yet. */
void module_system_init();

/* Exit every INITIALIZED module in reverse dependency order, then unload DLLs and clean up. */
void module_system_exit( void );

/*==============================================================================================
    Registration and loading
==============================================================================================*/

/* Register a statically-linked module.
   module_api must have func_api set to the module's exported API struct.
   Does NOT call init() — call module_init_all() once all modules are registered. */
bool module_static_load( const char* name, module_api_t* module_api ); // , void* exported_api

/* Load a DLL by base name (e.g. "render" → "<exe_dir>/render.dll").
   Creates a shadow copy, loads it, resolves exports, allocates state.
   Does NOT call init() — call module_init_all() once all modules are loaded. */
bool module_dynamic_load( const char* name );

/* Unload a single module: call exit(), free state, unload DLL, delete shadow file. */
bool module_unload( const char* name );

/* Hot-reload a single module in place:
   Call exit → unload DLL → shadow copy → load new DLL → Call on_reload() or init().
   State pointer is preserved; block is NOT zeroed on reload. */
bool module_reload( const char* name );

/*==============================================================================================
    Initialization and update
==============================================================================================*/

/* Topologically sort dependencies and call init() on every LOADED module.
   Returns false on a dependency cycle or if any init() returns false. */
bool module_init_all( void );

/* Tick every INITIALIZED module in dependency order. */
void module_system_tick( float dt );

/* Poll file timestamps and hot-reload any DLL that has changed on disk.
   Applies a debounce window (DEBOUNCE_MS) to avoid reloading while the linker
   is still writing the file. */
void module_check_reloads( void );

/*==============================================================================================
    API access
==============================================================================================*/

/* Returns the exported_api pointer for a named, INITIALIZED module, else NULL.
   Call the module spefici function render_api(), audio_api() for direct access.
   Cast the result to the module's typed struct:
       core_api_t*   core = module_get_api("core");
       render_api_t* rend = module_get_api("render");  */
const void* module_get_api( const char* name );

/* Last error message from any failed operation. */
const char* module_last_error( void );

/*==============================================================================================
    Debug
==============================================================================================*/

/* Debug helpers */
void module_list_all( void );

/*============================================================================================*/
#endif    // MODULE_SYS_H
/*==============================================================================================
 
    module notes:
 
    The three registration tiers are intentional and must stay explicit.
    Do not collapse them into a single macro — the distinction is load-bearing:
 
    ── Tier 1 ──────────────────────────────────────────────────────────────────────────────
    HOST SERVICES  —  module_static_load()
        Always compiled into the exe.  Never a DLL.  No BUILD_STATIC switch.
        Other modules acquire them through sys->get_api() in their init() callbacks.
        Registered first so they are in the table before any DLL load-time dep inspection.
 
            module_static_load( "core",  core_get_module_api() );
            module_static_load( "base",  base_get_module_api() );
            module_static_load( "jobs",  jobs_get_module_api() );
 
    ── Tier 2 ──────────────────────────────────────────────────────────────────────────────
    SWITCHABLE MODULES  —  module_load()  (macro)
        Statically linked in a monolithic build, hot-reloadable DLL in a dynamic build.
        The BUILD_STATIC flag controls which call the macro expands to.
        These are the only modules that participate in the dynamic/static build switch.
 
            module_load( renderer );
            module_load( audio );
            module_load( game );
 
    ── Tier 3 ──────────────────────────────────────────────────────────────────────────────
    FORCE-DYNAMIC  —  module_dynamic_load()  (rare, explicit)
        Reserved for modules that must be DLLs regardless of BUILD_STATIC — e.g. a
        scripting runtime or a platform plugin that ships as a separate binary.
 
            module_dynamic_load( "lua_runtime" );
 
    Both tiers are acquired identically at the call site:
        core_api()->log(...)            — zero-cost in exe-linked TUs
        renderer_api()->begin_frame()   — one load in dynamic builds
 
    ── Build commands ───────────────────────────────────────────────────────────────────────
    Monolithic (BUILD_STATIC, LTO):
        cc -DBUILD_STATIC -flto \
           main.c core.c base.c jobs.c renderer.c audio.c game.c module.c -o game
 
    Mixed (host services in exe, switchable modules as DLLs):
        Exe:
            cc -DCORE_LINK_STATIC -DBASE_LINK_STATIC -DJOBS_LINK_STATIC \
               main.c core.c base.c jobs.c module.c -o game
        DLLs:
            cc -shared renderer.c -o renderer.dll
            cc -shared audio.c    -o audio.dll
            cc -shared game.c     -o game.dll
 
==============================================================================================*/