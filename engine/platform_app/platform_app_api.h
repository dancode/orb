#ifndef PLATFORM_APP_API
#define PLATFORM_APP_API
/*============================================================================================*/

typedef struct platform_app_api_s platform_app_api_t;
typedef struct module_api_s       module_api_t;

module_api_t*       platform_app_get_module_api( void ); /* the lifecycle descriptor */
platform_app_api_t* platform_app_get_api( void );        /* the typed API struct    */

/*============================================================================================*/
#endif    // PLATFORM_APP_API