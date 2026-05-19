#ifndef MOD_API_H
#define MOD_API_H
/*==============================================================================================

    engine/mod/mod_api.h — mod module API struct and gateway macro.

    DLL modules that need to manage sub-modules (e.g. an editor loading plugins) include
    this and fetch mod's vtable via the standard gateway pattern:

        MOD_USE_MOD;                                 // file scope (dynamic builds only)
        if ( !MOD_FETCH_MOD ) return false;          // in init() / reload()
        mod()->dynamic_load( "my_plugin" );          // call site — identical in both modes

    Includes mod.h for the infrastructure macros (MOD_GATEWAY_*, MOD_FETCH_API, etc.)

==============================================================================================*/

#include "engine/mod/mod.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct mod_api_s
{
    bool        ( *dynamic_load )   ( const char* name );
    bool        ( *unload )         ( const char* name );
    const void* ( *get_api )        ( const char* name );
    bool        ( *reload )         ( const char* name );
    bool        ( *is_loaded )      ( const char* name );
    void        ( *each )           ( mod_visitor_fn visit, void* user );
    const char* ( *last_error )     ( void );

} mod_api_t;

/* mod is always exe-linked; DLLs in a dynamic build must still fetch via the registry. */
#if defined( BUILD_STATIC ) || defined( MOD_STATIC )
MOD_GATEWAY_STATIC( mod_api_t, mod )
#else
MOD_GATEWAY_DYNAMIC( mod_api_t, mod )
#endif

#if defined( BUILD_STATIC ) || defined( MOD_STATIC )
    #define MOD_USE_MOD    /* static build */
    #define MOD_FETCH_MOD  true
#else
    #define MOD_USE_MOD    MOD_DEFINE_API_PTR( mod_api_t, mod )
    #define MOD_FETCH_MOD  MOD_FETCH_API( mod_api_t, mod )
#endif

// clang-format on
/*============================================================================================*/
#endif    // MOD_API_H
