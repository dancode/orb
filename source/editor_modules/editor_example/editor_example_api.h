#ifndef EDITOR_EXAMPLE_API_H
#define EDITOR_EXAMPLE_API_H
/*==============================================================================================

    editor_modules/editor_example/editor_example_api.h — editor_example API struct and gateway.

==============================================================================================*/

#include "editor_modules/editor_example/editor_example.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct editor_example_api_s
{
    void ( *placeholder )( void );

} editor_example_api_t;

#if defined( BUILD_STATIC ) || defined( EDITOR_EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( editor_example_api_t, editor_example )
#else
MOD_GATEWAY_DYNAMIC( editor_example_api_t, editor_example )
#endif

#if defined( BUILD_STATIC ) || defined( EDITOR_EXAMPLE_STATIC )
    #define MOD_USE_EDITOR_EXAMPLE    /* static build */
    #define MOD_FETCH_EDITOR_EXAMPLE  true
#else
    #define MOD_USE_EDITOR_EXAMPLE    MOD_DEFINE_API_PTR( editor_example_api_t, editor_example )
    #define MOD_FETCH_EDITOR_EXAMPLE  MOD_FETCH_API( editor_example_api_t, editor_example )
#endif

// clang-format on
/*============================================================================================*/
#endif    // EDITOR_EXAMPLE_API_H
