#ifndef MODULE_API_H
#define MODULE_API_H
/*==============================================================================================

    module_api.h

    The only header module authors need to implement a module (DLL or static).

    Every module must provide two C functions:

        module_api_t*  name_get_module_api( void )   — lifecycle (init/tick/exit/on_reload)
        void*          name_get_api( void )           — the module's own typed API struct

    In dynamic builds these are resolved as undecorated DLL exports ("get_module_api",
    "get_api") via LoadLibrary.  In static builds they are called directly by the
    module_load() macro.

    State ownership
    ---------------
    The system allocates state_size bytes, zeroes the block on first load, and
    preserves it across hot-reloads.  Modules never free their own state

    Reload semantics
    ----------------
    on first load:   init()      is called with a zeroed state block.
    on hot-reload:   on_reload() is called if set; otherwise init() is called again.
                     State memory is PRESERVED in both cases.

    Use on_reload() to re-cache sibling API pointers that changed after a DLL swap.
    Use init() for first-time setup that should not repeat on reload.

==============================================================================================*/
#include "orb.h"

/* get_api: System interface passed into every module's init() and on_reload() call.

    The ONLY system symbol a DLL ever touches — it never links against the exe.
    At runtime the function pointer is passed so the module can resolve any registered API.

    Usage inside a DLL : core_api_t*    core   = sys->get_api( "core" );
                         render_api_t*  render = sys->get_api( "render" ); */

typedef const void* ( *get_api_fn )( const char* name );

/*==============================================================================================
    Module lifecycle callbacks
==============================================================================================*/

#define MODULE_MAX_DEPS 8

/* init : first-time setup.  state is zeroed on entry.  sys resolves sibling APIs. */
typedef bool ( *mod_init_fn )( void* state, get_api_fn get_api );

/* tick : called every frame while the module is INITIALIZED. */
typedef void ( *mod_tick_fn )( void* state, float dt );

/* exit : called before unload or reload.  Do NOT free state — the system owns it. */
typedef void ( *mod_exit_fn )( void* state );

/* on_reload : optional. Called INSTEAD of init() after a hot-reload.
               State memory arrives with its last values intact.
               Use to re-cache API pointers that became stale after the DLL swap.
               If NULL, init() is called on reload (state is NOT zeroed). */
typedef void ( *mod_on_reload_fn )( void* state, get_api_fn get_api );

/*==============================================================================================
    module_api_t — every module provides this via name_get_module_api()
==============================================================================================*/

typedef struct module_api_s
{
    int32_t          version;    /* bump when ABI changes */
    int32_t          state_size; /* bytes to allocate for persistent state; 0 = stateless */

    const char*      deps[ MODULE_MAX_DEPS ]; /* names of modules that must init before this one */
    int32_t          dep_count;

    const void*      func_api; /* typed API struct pointer — returned by module_get_api() */

    mod_init_fn      init;
    mod_tick_fn      tick;
    mod_exit_fn      exit;
    mod_on_reload_fn on_reload; /* optional — see reload semantics above */

} module_api_t;

typedef module_api_t* ( *get_module_api_fn )( void ); /* DLL export typedefs (dynamic builds only) */

/*==============================================================================================
    MODULE_DEFINE_ACCESS_FUNC — declares the API access inline function in a consuming header.

    Place macro in the API header (e.g. render_api.h) after the API struct is declared.
    It declares the inline getter that all consumers call: render_api(), audio_api(), etc.

    --- STATIC BUILD ---

        This depends on the provider .c defining a global struct with the expected name:

        const render_api_t g_render_api_struct = { .draw = internal_draw, ... };

        The MODULE_DEFINE_ACCESS_FUNC declares the struct extern and returns its address.
        The struct is a single globally-visible constant, LTO can see through
        render_api()->draw(dt) and devirtualize it to a direct call to internal_draw(dt)
        with no indirection.

    --- DYNAMIC BUILD ---

        The depends on the provider .c (DLL) defining a local pointer:

        const render_api_t* g_render_api_ptr = NULL;   // via MODULE_DEFINE_API_PTR

        The pointer is populated once in init() via get_api("render"), then all calls
        to render_api() are a single pointer load — one extra indirection vs static, which
        is the correct trade-off for a hot-reloadable build.

    Usage in an API header:

        typedef struct render_api_s { void (*draw)(float dt); } render_api_t;
        MODULE_GATEWAY( render_api_t, render )

    Usage in consumer code (call site, identical in both builds):

        render_api()->draw( dt );

==============================================================================================*/

/*==============================================================================================
    MODULE_GATEWAY_STRUCT_PATH / MODULE_GATEWAY_PTR_PATH
    Primitive helpers used by both gateway variants.  Not for direct use.

    1. Provide access to global struct in the provider's .c; resolved at link time.
    2. Provide acceess to global pointer resolved at runtime via get_api.

==============================================================================================*/

#define MODULE_GATEWAY_STRUCT_PATH( type, name )     \
    extern const type         g_##name##_api_struct; \
    static inline const type* name##_api( void ) { return &g_##name##_api_struct; }

#define MODULE_GATEWAY_PTR_PATH( type, name )     \
    extern const type*        g_##name##_api_ptr; \
    static inline const type* name##_api( void ) { return g_##name##_api_ptr; }

/*==============================================================================================
    MODULE_GATEWAY — standard, build-variant-aware gateway.
    Place once in the module's API header after the struct typedef.
==============================================================================================*/

#ifdef BUILD_STATIC
#    define MODULE_GATEWAY( type, name ) MODULE_GATEWAY_STRUCT_PATH( type, name )
#else
#    define MODULE_GATEWAY( type, name ) MODULE_GATEWAY_PTR_PATH( type, name )
#endif

/*==============================================================================================
    MODULE_GATEWAY_ALWAYS_STATIC — gateway for modules that live in the exe at all times.

    Cannot evaluate an arbitrary link-flag symbol inside a macro body.  Instead, expose
    both paths as named macros and let the API header choose with a plain #ifdef.

    In the provider's API header (e.g. core_api.h), write:

        #if defined( BUILD_STATIC ) || defined( CORE_LINK_STATIC )
            MODULE_GATEWAY_STRUCT_PATH( core_api_t, core )   // exe-linked: zero cost
        #else
            MODULE_GATEWAY_PTR_PATH( core_api_t, core )      // DLL-linked: one load
        #endif

    The BUILD_STATIC arm covers a full monolithic build.
    The CORE_LINK_STATIC arm covers mixed builds where this TU is compiled into the exe.
    The else arm covers DLL translation units that consume core via a cached pointer.

==============================================================================================*/

/* (The two helpers above are the building blocks; see core_api.h for the full pattern.) */


/*==============================================================================================
    MODULE_DEFINE_API_PTR — allocates cached pointer storage in a consuming .c file.
    Expands to nothing in static builds (struct is linked in directly).

    Place once at file scope in any .c that consumes a sibling API as a DLL:
        MODULE_DEFINE_API_PTR( core_api_t, core );

==============================================================================================*/

#ifdef BUILD_STATIC
#    define MODULE_DEFINE_API_PTR( type, name ) /* struct linked directly — no pointer needed */
#else
#    define MODULE_DEFINE_API_PTR( type, name ) const type* g_##name##_api_ptr = NULL
#endif

/*==============================================================================================
    MODULE_GET_API — populates the cached pointer inside init() or on_reload().
    Expands to (1) in static builds (struct is linked, nothing to fetch).

    Usage:
        if ( !MODULE_FETCH_API( core_api_t, core ) ) return false;
==============================================================================================*/

#ifdef BUILD_STATIC
#    define MODULE_GET_API( type, name ) ( 1 ) /* always succeeds — struct is linked */
#else
#    define MODULE_GET_API( type, name ) ( ( g_##name##_api_ptr = ( const type* )get_api( #name ) ) != NULL )
#endif

/*==============================================================================================
    MODULE_EXPORT_DECL — marks DLL exports on platforms that require it.
    Expands to nothing on ELF targets (GCC/Clang visibility handles this).
==============================================================================================*/

#if defined( _WIN32 ) && !defined( BUILD_STATIC )
#    define MODULE_EXPORT __declspec( dllexport )
#else
#    define MODULE_EXPORT
#endif

/*==============================================================================================
    MODULE_DEFINE_EXPORTS — emits the undecorated "get_module_api" / "get_api" symbols
    that the module system resolves via LoadLibrary / dlopen.

    Place once at the bottom of the module's .c, outside any function: MODULE_DEFINE_EXPORTS( render )
==============================================================================================*/

#ifdef BUILD_STATIC
#    define MODULE_DEFINE_EXPORTS( name ) /* static builds: system calls name##_get_module_api directly */
#else
#    define MODULE_DEFINE_EXPORTS( name )                                                      \
        MODULE_EXPORT module_api_t* get_module_api( void ) { return name##_get_module_api(); }
#endif

/*==============================================================================================
    MODULE_NEVER_DYNAMIC — safety assertion for always-static modules.
    Place at the bottom of core.c, base.c, etc. instead of MODULE_DEFINE_EXPORTS.
    Triggers a compile error if the file is accidentally compiled as a DLL.
==============================================================================================*/

#ifdef BUILD_STATIC
#    define MODULE_NEVER_DYNAMIC( name )
#else
#    define MODULE_NEVER_DYNAMIC( name ) \
        _Static_assert( 0, #name " is an always-static module — do not build it as a DLL" )
#endif

/*============================================================================================*/
#endif    // MODULE_API_H