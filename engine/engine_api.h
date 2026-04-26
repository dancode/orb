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

typedef struct platform_app_api_s platform_app_api_t;
typedef struct module_api_s       module_api_t;

module_api_t*               engine_get_module_api( void ); /* the lifecycle descriptor */
engine_api_t*               engine_get_api( void );        /* the typed API struct    */


/*============================================================================================*/
#endif    // ENGINE_API_H