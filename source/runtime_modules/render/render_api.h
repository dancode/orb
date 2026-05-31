#ifndef RENDER_API_H
#define RENDER_API_H
/*==============================================================================================

    runtime_modules/render/render_api.h — render module API struct and gateway macro.

==============================================================================================*/

#include "runtime_modules/render/render.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct render_api_s
{
    void ( *begin_frame )( void );
    void ( *draw_frame )( float dt );
    void ( *end_frame )( void );
    int  ( *frame_count )( void );
    void ( *set_clear_color )( float r, float g, float b, float a );

} render_api_t;

#if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
    MOD_GATEWAY_STATIC( render_api_t, render )
#else
    MOD_GATEWAY_DYNAMIC( render_api_t, render )
#endif

#if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
    #define MOD_USE_RENDER    /* static build */
    #define MOD_FETCH_RENDER  true
#else
    #define MOD_USE_RENDER    MOD_DEFINE_API_PTR( render_api_t, render )
    #define MOD_FETCH_RENDER  MOD_FETCH_API( render_api_t, render )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RENDER_API_H
