#ifndef ENGINE_API_H
#define ENGINE_API_H
/*==============================================================================================

    engine_api.h

==============================================================================================*/

#include "engine/module/module_api.h"

typedef struct engine_api_s
{
    void ( *print )( const char* );
    int ( *should_quit )( void );

} engine_api_t;

MODULE_GATEWAY( engine_api_t, engine );

// struct module_api_s* engine_get_module_api( void );
void*    engine_get_api( void );

/*============================================================================================*/
#endif    // ENGINE_API_H