#ifndef EXAMPLE_API_H
#define EXAMPLE_API_H
/*==============================================================================================

    example_api.h : Example module API header

==============================================================================================*/

#include "engine/mod/mod_api.h"

/*==============================================================================================
    Example API struct
==============================================================================================*/

typedef struct example_api_s
{
    void ( *example_function_1 )( void );
    void ( *example_function_2 )( int value );

} example_api_t;

#if defined( BUILD_STATIC ) || defined( EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( example_api_t, example )
#else
MOD_GATEWAY_DYNAMIC( example_api_t, example )
#endif

/*============================================================================================*/
#endif    // EXAMPLE_API_H
