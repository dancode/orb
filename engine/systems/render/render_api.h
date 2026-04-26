#ifndef RENDER_API_H
#define RENDER_API_H

/*==============================================================================================

    render_api.h

==============================================================================================*/

typedef struct render_api_s
{
    void ( *draw_frame )( float dt );
    void ( *set_clear_color )( float r, float g, float b );

} render_api_t;

/* return type is const — callers cannot modify the table */
const render_api_t* get_render_api( void );

/*============================================================================================*/
#endif    // RENDER_API_H