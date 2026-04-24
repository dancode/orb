#ifndef ENGINE_API_H
#define ENGINE_API_H
/*==============================================================================================

    engine_api.h

==============================================================================================*/

typedef struct engine_api_s
{
    void ( *print )( const char* );
    int ( *should_quit )( void );    
    // double ( *time_get_fn )( void );
    
} engine_api_t;

/*============================================================================================*/
/* public core api functions */

void          engine_api_init( void );
void          engine_api_exit( void );
engine_api_t* engine_get_api( void );

/*============================================================================================*/
#endif    // ENGINE_API_H