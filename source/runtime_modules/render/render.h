#ifndef RENDER_H
#define RENDER_H
/*==============================================================================================

    render.h — Public API exported by the renderer module.

    Consumers include this header and call render_api()->begin_frame() etc.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct render_api_s
{
    void ( *begin_frame )( void );
    void ( *draw_frame )( float dt );
    void ( *end_frame )( void );
    int  ( *frame_count )( void );
    void ( *set_clear_color )( float r, float g, float b );

} render_api_t;

#if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
MOD_GATEWAY_STATIC( render_api_t, render )
#else
MOD_GATEWAY_DYNAMIC( render_api_t, render )
#endif

/*============================================================================================*/
#endif    // RENDER_H
