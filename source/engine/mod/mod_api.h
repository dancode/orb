#ifndef MOD_API_H
#define MOD_API_H
/*==============================================================================================

    mod_api.h

    Modules declaration
    -------------------
    The only header module authors need to implement a module (DLL or static).
    Every module must provide two custom "module name" derived C functions:

        mod_api_t*  name_get_mod_api( void )   — lifecycle (init/tick/exit/on_reload)
        void*          name_get_api( void )          — the module's own typed API struct

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
#include "orb.h"

/*  The system interface passed into every module's init() and on_reload() call.

    The ONLY system symbol a DLL ever touches — it never links against the exe.
    At runtime the pointer is passed so the module can resolve any registered API.

    Called only inside a DLL's init() or reload() function.

        if ( !MOD_FETCH_API( core_api_t, core ))

    In static builds this does nothing. In dynamic builds it resolves the API pointer
    from the module system "get_api("name") and caches it in a global for subsequent calls. */

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

    Macros To Pivot On Dynamic vs Static Builds
    -------------------------------------------

    Place GATEWAY macro in the API header (e.g. render_api.h) after the API struct is declared.
    It declares the inline getter that all consumers call: render_api(), audio_api(), etc.

    MOD_GATEWAY_STATIC (static) or MOD_GATEWAY_DYNAMIC (dynamic)

    --- STATIC BUILD ---

        The provider module (.c) must define a global (non-static) struct with expected name:

            const <name>_api_t g_<name>_api_struct = { .func = internal_func, ... };

        The global access function declares the the struct as extern and inlines a return.
        The struct is a single globally-visible constant so LTO can devirtualize.
        render_api()->draw(dt) bcomes a direct call to internal_draw(dt) with no indirection.

    --- DYNAMIC BUILD ---

        The provider module (.c) must define a local pointer.

            const <name>_api_t* g_<name>_api_ptr = NULL;

        The pointer is populated at init() via MOD_FETCH_API( <name>_api_t, <name> )
        All calls to <name>_api() are a single pointer load, one extra indirection vs static.

    -- USAGE IN API HEADER --

            typedef struct <name>_api_s { void (*func)(float arg); } <name>_api_t;

            (always static) MOD_GATEWAY_STATIC( <name>_api_t, <name> ) OR
            (always dynamic) MOD_GATEWAY_DYNAMIC( <name>_api_t, <name> )

    Usage in consumer code (call site, identical in both builds):

            <name>_api()->func( argument );

==============================================================================================*/

/* STATIC: every TU sees the struct directly. LTO can devirtualize the call. */
#define MOD_GATEWAY_STATIC( type, name )             \
    extern const type         g_##name##_api_struct; \
    static inline const type* name##_api( void ) { return &g_##name##_api_struct; }

/* DYNAMIC: every TU reads through a single pointer. Populated post-init. */
#define MOD_GATEWAY_DYNAMIC( type, name )         \
    extern const type*        g_##name##_api_ptr; \
    static inline const type* name##_api( void ) { return g_##name##_api_ptr; }

/*==============================================================================================

    MODULE_GATEWAY (STATIC SWITCH) — Gateway for modules that live in the exe at all times.

    When declaring a module that is always static (e.g. core.c), the API struct is linked
    in directly to the exectuable that compiled it in.

    External DLL's still need to cache a pointer to the API struct. In this case they will
    see the external pointer declaration from MOD_GATEWAY_DYNAMIC, but the module's init()
    will populate that pointer.

    #if defined( BUILD_STATIC ) || defined( CORE_STATIC )
            MOD_GATEWAY_STATIC( core_api_t, core )          // EXE-linked: zero cost
        #else
            MOD_GATEWAY_DYNAMIC( core_api_t, core )         // DLL-linked: one load
        #endif

    The BUILD_STATIC arm covers a full monolithic build (even DLL's get struct)
    The CORE_STATIC arm covers mixed builds where this TU is compiled into the exe.
    The else arm covers DLL that store a local cached consmer pointer.

================================================================================================

    MOD_DEFINE_API_PTR — allocates cached pointer storage in a consuming .c file.
    Expands to nothing in static builds (struct is linked in directly).

    Place once at file scope in any .c that consumes a sibling API as a DLL:
        MOD_DEFINE_API_PTR( core_api_t, core );

==============================================================================================*/

#ifdef BUILD_STATIC
    #define MOD_DEFINE_API_PTR( type, name ) /* struct linked directly — no pointer needed */
#else
    #define MOD_DEFINE_API_PTR( type, name ) const type* g_##name##_api_ptr = NULL
#endif

/*==============================================================================================
    MOD_FETCH_API — Populates the cached pointer inside init() or on_reload().
                   - Redundant in static builds (struct is linked, nothing to fetch).
    Usage:
                     if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
==============================================================================================*/

#ifdef BUILD_STATIC
    #define MOD_FETCH_API( type, name ) ( 1 ) /* always succeeds — struct is linked */
#else
    #define MOD_FETCH_API( type, name ) ( ( g_##name##_api_ptr = ( const type* )get_api( #name ) ) != NULL )
#endif

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

#ifdef BUILD_STATIC
    #define MOD_DEFINE_EXPORTS( name ) /* static builds: system calls name##_get_mod_api directly */
#else
    #define MOD_DEFINE_EXPORTS( name ) \
        MOD_EXPORT mod_api_t* get_mod_api( void ) { return name##_get_mod_api(); }
#endif

/*============================================================================================*/
#endif    // MOD_API_H