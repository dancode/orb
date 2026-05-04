#ifndef app_API
#define app_API
/*============================================================================================*/

typedef struct app_api_s    app_api_t;
typedef struct module_api_s module_api_t;

module_api_t*               app_get_module_api( void ); /* the lifecycle descriptor */
app_api_t*                  app_get_api( void );        /* the typed API struct    */


// #if defined( ORB_BUILD_STATIC ) || defined( APP_LINK_STATIC )
// MODULE_GATEWAY_STRUCT_PATH( app_api_t, app )
// #else
// MODULE_GATEWAY_PTR_PATH( app_api_t, app )
// #endif


/*============================================================================================*/
#endif    // app_API