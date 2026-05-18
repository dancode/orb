#ifndef EXAMPLE_H
#define EXAMPLE_H
/*==============================================================================================

    example.h — Public API exported by the example module.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct example_api_s
{
    void ( *example_function_1 )( void );
    void ( *example_function_2 )( int value );
    void ( *fail_next_reload )( void ); /* test helper: makes the next on_reload return false */
    void ( *update )( float dt );

} example_api_t;

#if defined( BUILD_STATIC ) || defined( EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( example_api_t, example )
mod_desc_t* example_get_mod_desc( void );
#else
MOD_GATEWAY_DYNAMIC( example_api_t, example )
#endif

#if defined( BUILD_STATIC ) || defined( EXAMPLE_STATIC )
    #define MOD_USE_EXAMPLE    /* static build */
    #define MOD_FETCH_EXAMPLE  true
#else
    #define MOD_USE_EXAMPLE    MOD_DEFINE_API_PTR( example_api_t, example )
    #define MOD_FETCH_EXAMPLE  MOD_FETCH_API( example_api_t, example )
#endif

/*============================================================================================*/
#endif    // EXAMPLE_H
