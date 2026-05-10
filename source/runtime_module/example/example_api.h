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
    void ( *fail_next_reload )( void ); /* test helper: makes the next on_reload return false */

} example_api_t;

#if defined( BUILD_STATIC ) || defined( EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( example_api_t, example )
mod_api_t* example_get_mod_api( void );
#else
MOD_GATEWAY_DYNAMIC( example_api_t, example )
#endif

/*============================================================================================*/
#endif    // EXAMPLE_API_H
