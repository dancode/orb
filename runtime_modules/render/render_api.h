#ifndef RENDER_API_H
#define RENDER_API_H

/*==============================================================================================

    render_api.h

    Public API exported by the renderer module (typically a hot-reloadable DLL).

    Consumers include this header and call render_api()->begin_frame() etc.

==============================================================================================*/

#include "module/module_api.h"

/*==============================================================================================
    Renderer API struct
==============================================================================================*/

typedef struct render_api_s
{
    void ( *draw_frame )( float dt );
    void ( *set_clear_color )( float r, float g, float b );

    // void ( *begin_frame )( void );
    // void ( *draw_quad )( float x, float y, float w, float h, unsigned int rgba );
    // void ( *end_frame )( void );
    // int ( *draw_call_count )( void );

} render_api_t;

#if defined( BUILD_STATIC ) || defined( RENDER_LINK_STATIC )
MODULE_GATEWAY_STRUCT_PATH( render_api_t, render )
#else
MODULE_GATEWAY_PTR_PATH( render_api_t, render )
#endif

/*============================================================================================*/

// #ifdef BUILD_STATIC
//
// extern const render_api_t g_render_api_struct;
//
// static inline const render_api_t*
// render_api( void )
// {
//     return &g_render_api_struct;
// }
//
// #else
//
// extern const render_api_t* g_render_api_ptr;
//
// static inline const render_api_t*
// render_api( void )
// {
//     return g_render_api_ptr;
// }
//
// #endif

/*============================================================================================*/
#endif    // RENDER_API_H