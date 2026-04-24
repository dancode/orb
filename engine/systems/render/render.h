#ifndef RENDER_H
#define RENDER_H

/*==============================================================================================

    engine_api.h

==============================================================================================*/

typedef struct render_api_s
{
    int ( *get_framecount )( void );
    void ( *render_print )( const char* );
    float ( *add )( float, float );

} render_api_t;

/*============================================================================================*/
#endif    // RENDER_H