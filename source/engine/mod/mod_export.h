/*==============================================================================================

    engine/mod_export.h : The module export API struct and lifecycle callback typedefs.

    Purpose
    -------
    This is the only header a module author needs to implement a module, whether static or dynamic.
    It defines the mod_api_t struct that every module must return from its get_mod_api() export,
    and the lifecycle callback typedefs (init/tick/exit/reload) that the system calls.

    Includes the MOD_EXPORT / MOD_DEFINE_EXPORTS macros for DLL exports.

    Usage
    -----
    Only include this header when implementing a module, never in the host exe or other modules.
    The host and other modules only include mod_api.h to get the API struct definitions and
    consumer-side macros.

    Modules declaration
    -------------------

    To implement a module (DLL or static). Every module must provide two custom 
    "module name" derived C function:

        mod_api_t*  name_get_mod_api( void )  — lifecycle (init/tick/exit/on_reload)
        mod_api_t*  get_mod_api( void )       - exported accessor for dynamic builds

    In dynamic builds these are resolved as undecorated DLL exports via LoadLibrary:

        MOD_DEFINE_EXPORTS( module_name );

    This defines a single generic export "get_mod_api" whose implementation calls
    the module-specific name_get_mod_api() internally.

    In static builds mod_static_load() is used to get the module api and the
    lifecycle callbacks and state size. Since there are no DLLs, the module's own API
    struct is linked in directly and returned by name_get_api() with no indirection.

    State ownership
    ---------------
    The system allocates state_size bytes, zeroes the block on first load, and
    preserves it across hot-reloads.  Modules never free their own state

    Reload semantics
    ----------------
    on first load:   init()      is called with a zeroed state block.
    on hot-reload:   on_reload() is called (must exist for hot-reload)

                     State memory is PRESERVED in both cases.

    Use on_reload() to re-cache sibling API pointers that changed after a DLL swap.
    Use init() for first-time setup that should not repeat on reload.

==============================================================================================*/
#ifndef MOD_EXPORT_H
#define MOD_EXPORT_H

#include "orb.h"
#include "engine/mod/mod_api.h"

/*==============================================================================================

    The system interface passed into every module's init() and on_reload() call.

    The ONLY system symbol a DLL ever touches — it never links against the exe.
    At runtime the pointer is passed so the module can resolve any registered API.

    Called only inside a DLL's init() or reload() function.

        if ( !MOD_FETCH_API( core_api_t, core ))

    In static builds this does nothing. In dynamic builds it resolves the API pointer
    from the module system "get_api("name") and caches it in a global for subsequent calls.

==============================================================================================*/

typedef const void* ( *get_api_fn )( const char* name );

/*==============================================================================================
    Module lifecycle callbacks
==============================================================================================*/

#define MOD_MAX_DEPS 8

/* init : first-time setup, state is zeroed on entry, and get_api resolves APIs. */
typedef bool ( *mod_init_fn )( void* state, get_api_fn get_api );

/* tick : called every frame while the module is INITIALIZED. */
typedef void ( *mod_tick_fn )( void* state, float dt );

/* exit : called before unload or reload.  Do NOT free state — the system owns it. */
typedef void ( *mod_exit_fn )( void* state );

/* reload : required for hot-reload, called INSTEAD of init(). state memory is preserved. */
typedef void ( *mod_reload_fn )( void* state, get_api_fn get_api );

/*==============================================================================================
    mod_api_t — every module provides this via name_get_mod_api()
==============================================================================================*/

typedef struct mod_api_s
{
    int32_t       version;              /* bump when ABI changes */
    int32_t       state_size;           /* persistent state size or 0 = stateless */
    const char*   deps[ MOD_MAX_DEPS ]; /* names of modules to init before this one */
    int32_t       dep_count;            /* number of dependencies in deps[] */
    const void*   func_api;             /* API struct pointer returned by MOD_FETCH_API() */

    mod_init_fn   init;
    mod_tick_fn   tick;
    mod_exit_fn   exit;
    mod_reload_fn reload;

} mod_api_t;

/* DLL API export function typedef (exported in dynamic builds only) */
typedef mod_api_t* ( *get_mod_api_fn )( void );

/*==============================================================================================
    MOD_EXPORT — Marks DLL exports on platforms that require it.
==============================================================================================*/

#if defined( _WIN32 ) && !defined( BUILD_STATIC )
    #define MOD_EXPORT __declspec( dllexport )
#else
    #define MOD_EXPORT
#endif

/*==============================================================================================
    MOD_DEFINE_EXPORTS — emits the undecorated "get_mod_api" / "get_api" symbols
    that the module system resolves via LoadLibrary / dlopen.

    Place once at the bottom of the module's .c, outside any function: MOD_DEFINE_EXPORTS( render )
==============================================================================================*/
// void* name##_get_api( void ) { return ( void* )&g_##name##_api_struct; }

#ifdef BUILD_STATIC
    #define MOD_DEFINE_EXPORTS( name ) /* static builds: system calls name##_get_mod_api directly */
#else
    #define MOD_DEFINE_EXPORTS( name ) \
        MOD_EXPORT mod_api_t* get_mod_api( void ) { return name##_get_mod_api(); }
#endif

/*============================================================================================*/
#endif    // MOD_EXPORT_H
