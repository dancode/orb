#ifndef MOD_IMPORT_H
#define MOD_IMPORT_H
/*==============================================================================================

    engine/mod/mod_import.h — Consumer-side module API access macros.

    Supports both static and dynamic builds with identical call sites. The build mode is
    controlled by BUILD_STATIC (monolithic) or per-module <NAME>_STATIC (exe-linked service).

    Header file markup pattern (place in modules API header)
    --------------------------------------------------------

    MOD_GATEWAY_STATIC( type, name )
        Static build: declares g_<name>_api_struct as extern and returns its address directly.
        LTO can devirtualize: render()->draw(dt) becomes a direct call with no indirection.

        Provider .c must define:
            const <name>_api_t g_<name>_api_struct = { .func = internal_func, ... };

    MOD_GATEWAY_DYNAMIC( type, name )
        Dynamic build: declares g_<name>_api_ptr and returns it through a single pointer load.

        Provider .c must get the pointer in init() via MOD_FETCH_API.

    Both macros emit an identical inline accessor: <name>()
    Call sites are identical in both builds: render()->begin_frame()

    Example: 
    
        #if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
            MOD_GATEWAY_STATIC( render_api_t, render )   // EXE-linked: zero cost
        #else
            MOD_GATEWAY_DYNAMIC( render_api_t, render )  // DLL-linked: one pointer load
        #endif

    BUILD_STATIC covers a full monolithic build and RENDER_STATIC covers mixed builds
    where this module is compiled into the exe but also used from a DLL that caches 
    a pointer fetched during init().

==============================================================================================*/

#include "orb.h"

/* Forward declaration — full definition is in mod_export.h / mod_host.h. */
typedef struct mod_desc_s mod_desc_t;

/* STATIC: every TU sees the struct directly. LTO can devirtualize the call. */
#define MOD_GATEWAY_STATIC( type, name )                                            \
                                                                                    \
    typedef struct mod_desc_s   mod_desc_t;                                         \
    extern const type           g_##name##_api_struct;                              \
    static inline const type*   name( void ) { return &g_##name##_api_struct; }     \
    mod_desc_t*                 name##_get_mod_desc( void );

/* DYNAMIC: every TU reads through a cached pointer populated during init(). */
#define MOD_GATEWAY_DYNAMIC( type, name )                                           \
                                                                                    \
    typedef struct mod_desc_s   mod_desc_t;                                         \
    extern const type*          g_##name##_api_ptr;                                 \
    static inline const type*   name( void ) { return g_##name##_api_ptr; }

/*==============================================================================================
    MOD_DEFINE_API_PTR — Allocates cached pointer storage in a consuming .c file.

    Place once at file scope in any .c that consumes a module API as a DLL:
        MOD_DEFINE_API_PTR( core_api_t, core );

    Expands to nothing in static builds (struct linked directly — no pointer needed).
==============================================================================================*/

#if defined( BUILD_STATIC ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    #define MOD_DEFINE_API_PTR( type, name ) /* struct linked directly from header extern */
#else
    #define MOD_DEFINE_API_PTR( type, name ) const type* g_##name##_api_ptr = NULL
#endif

/*==============================================================================================
    MOD_FETCH_API — Populates the cached pointer inside init() or reload().

    Always succeeds in static builds (struct is linked; nothing to fetch).

    Usage: if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
==============================================================================================*/

#if defined( BUILD_STATIC ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    #define MOD_FETCH_API( type, name ) ( 1 ) /* struct linked directly from header extern */
#else
    #define MOD_FETCH_API( type, name ) ( ( g_##name##_api_ptr = ( const type* )get_api( #name ) ) != NULL )
#endif

/*==============================================================================================
    mod_visitor_fn — callback for mod_each; used by mod_api_t and mod_host.h.
==============================================================================================*/

typedef void ( *mod_visitor_fn )( const char* name, const mod_desc_t* api, void* user );

/*============================================================================================*/
#endif    // MOD_IMPORT_H
