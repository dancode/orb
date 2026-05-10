#ifndef MOD_API_H
#define MOD_API_H
/*==============================================================================================

    mod_api.h : Consumer-side module API access macros.

    Supports both static and dynamic builds with identical call sites. The build mode is
    controlled by BUILD_STATIC (monolithic) or per-module <NAME>_STATIC (exe-linked service).

   MOD_GATEWAY_STATIC( type, name )
        Static build: declares g_<name>_api_struct as extern and returns its address directly.
        LTO can devirtualize: render_api()->draw(dt) becomes a direct call with no indirection.

        Provider .c must define:
            const <name>_api_t g_<name>_api_struct = { .func = internal_func, ... };

    MOD_GATEWAY_DYNAMIC( type, name )
        Dynamic build: declares g_<name>_api_ptr and returns it through a single pointer load.

        Provider .c must populate the pointer in init() via MOD_FETCH_API.

    Both macros emit an identical inline accessor: <name>_api()
    Call sites are identical in both builds:  render_api()->begin_frame()

    Static switch pattern (place in the module's API header)
    ---------------------------------------------------------
        #if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
            MOD_GATEWAY_STATIC( render_api_t, render )   // exe-linked: zero cost
        #else
            MOD_GATEWAY_DYNAMIC( render_api_t, render )  // DLL-linked: one pointer load
        #endif

    BUILD_STATIC covers a full monolithic build.
    RENDER_STATIC covers mixed builds where this module is compiled into the exe.
    The else arm covers DLLs that cache a pointer fetched during init().

==============================================================================================*/
#include "orb.h"

/* STATIC: every TU sees the struct directly. LTO can devirtualize the call. */
#define MOD_GATEWAY_STATIC( type, name )             \
    typedef struct mod_api_s  mod_api_t; \
    extern const type         g_##name##_api_struct; \
    static inline const type* name##_api( void ) { return &g_##name##_api_struct; } \
    mod_api_t*                name##_get_mod_api( void );

/* DYNAMIC: every TU reads through a cached pointer populated during init(). */
#define MOD_GATEWAY_DYNAMIC( type, name )         \
    typedef struct mod_api_s mod_api_t; \
    extern const type*        g_##name##_api_ptr; \
    static inline const type* name##_api( void ) { return g_##name##_api_ptr; }

/*==============================================================================================
    MOD_DEFINE_API_PTR — Allocates cached pointer storage in a consuming .c file.

    Place once at file scope in any .c that consumes a module API as a DLL:
        MOD_DEFINE_API_PTR( core_api_t, core );

    Expands to nothing in static builds (struct linked directly — no pointer needed).
==============================================================================================*/

#ifdef BUILD_STATIC
    #define MOD_DEFINE_API_PTR( type, name ) /* struct linked directly — no pointer needed */
#else
    #define MOD_DEFINE_API_PTR( type, name ) const type* g_##name##_api_ptr = NULL
#endif

/*==============================================================================================
    MOD_FETCH_API — Populates the cached pointer inside init() or reload().

    Always succeeds in static builds (struct is linked; nothing to fetch).

    Usage:
        if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
==============================================================================================*/

#ifdef BUILD_STATIC
    #define MOD_FETCH_API( type, name ) ( 1 ) /* always succeeds — struct is linked */
#else
    #define MOD_FETCH_API( type, name ) ( ( g_##name##_api_ptr = ( const type* )get_api( #name ) ) != NULL )
#endif

/* Host-local variant — fetches via mod_get_api() (valid outside a module init() callback). */

#ifdef BUILD_STATIC
    #define HOST_FETCH_API( type, name ) ( 1 )
#else
    #define HOST_FETCH_API( type, name ) \
        ( ( g_##name##_api_ptr = ( const type* )mod_get_api( #name ) ) != NULL )
#endif

/*============================================================================================*/
#endif    // MOD_API_H