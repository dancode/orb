#ifndef IMGUI_API_H
#define IMGUI_API_H
/*==============================================================================================

    runtime_modules/imgui/imgui_api.h -- imgui module API struct and gateway macro.
    hot-reloadable DLL; BUILD_STATIC switches to static gateway.

==============================================================================================*/

#include "runtime_modules/imgui/imgui.h"
#include "engine/mod/mod_import.h"

/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct imgui_api_s
{
    void ( *tick )( float dt );    /* TODO: replace with real API functions */

} imgui_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( IMGUI_STATIC )
    MOD_GATEWAY_STATIC( imgui_api_t, imgui )
#else
    MOD_GATEWAY_DYNAMIC( imgui_api_t, imgui )
#endif

#if defined( BUILD_STATIC ) || defined( IMGUI_STATIC )
    #define MOD_USE_IMGUI    /* static build */
    #define MOD_FETCH_IMGUI  true
#else
    #define MOD_USE_IMGUI    MOD_DEFINE_API_PTR( imgui_api_t, imgui )
    #define MOD_FETCH_IMGUI  MOD_FETCH_API( imgui_api_t, imgui )
#endif

/*============================================================================================*/
#endif    // IMGUI_API_H
