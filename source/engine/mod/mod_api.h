#ifndef MOD_API_H
#define MOD_API_H
/*==============================================================================================

    mod_api.h : API access macros for static and dynamic builds.

    The module system supports both static and dynamic builds with zero code changes to the
    consumer.  The build mode is determined by BUILD_STATIC, which controls how the API
    struct is exposed by the provider module and how the consumer accesses it.

==============================================================================================*/
#include "orb.h"
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

            (always static)  MOD_GATEWAY_STATIC( <name>_api_t, <name> ) OR
            (always dynamic) MOD_GATEWAY_DYNAMIC( <name>_api_t, <name> )

    Usage in consumer code (call site, identical in both builds):

            <name>_api()->func( argument );

==============================================================================================*/

/* STATIC: every TU sees the struct directly. LTO can devirtualize the call. */
#define MOD_GATEWAY_STATIC( type, name )             \
    typedef struct mod_api_s  mod_api_t; \
    extern const type         g_##name##_api_struct; \
    static inline const type* name##_api( void ) { return &g_##name##_api_struct; } \
    mod_api_t*                name##_get_mod_api( void );

/* DYNAMIC: every TU reads through a single pointer. Populated post-init. */
#define MOD_GATEWAY_DYNAMIC( type, name )         \
    typedef struct mod_api_s mod_api_t; \
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

/* Host-local helper — fetches via mod_get_api (works outside an init() callback) */

#ifdef BUILD_STATIC
    #define HOST_FETCH_API( type, name ) ( 1 )
#else
    #define HOST_FETCH_API( type, name ) \
        ( ( g_##name##_api_ptr = ( const type* )mod_get_api( #name ) ) != NULL )
#endif

/*============================================================================================*/
#endif    // MOD_API_H