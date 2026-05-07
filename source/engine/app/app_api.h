#ifndef app_API
#define app_API
/*============================================================================================*/
#include "engine/mod/mod_api.h"

typedef struct app_api_s
{
    void ( *app_function )( void );

} app_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( APP_STATIC )
MOD_GATEWAY_STATIC( app_api_t, app )
#else
MOD_GATEWAY_DYNAMIC( app_api_t, app )
#endif

/*============================================================================================*/
#endif    // app_API