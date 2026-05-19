#ifndef EDITOR_API_H
#define EDITOR_API_H
/*==============================================================================================

    editor/editor_api.h — editor module API struct and gateway macro.

==============================================================================================*/

#include "editor/editor.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct editor_api_s
{
    void ( *placeholder )( void );

} editor_api_t;

#if defined( BUILD_STATIC ) || defined( EDITOR_STATIC )
MOD_GATEWAY_STATIC( editor_api_t, editor )
#else
MOD_GATEWAY_DYNAMIC( editor_api_t, editor )
#endif

#if defined( BUILD_STATIC ) || defined( EDITOR_STATIC )
    #define MOD_USE_EDITOR    /* static build */
    #define MOD_FETCH_EDITOR  true
#else
    #define MOD_USE_EDITOR    MOD_DEFINE_API_PTR( editor_api_t, editor )
    #define MOD_FETCH_EDITOR  MOD_FETCH_API( editor_api_t, editor )
#endif

// clang-format on
/*============================================================================================*/
#endif    // EDITOR_API_H
