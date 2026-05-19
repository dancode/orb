#ifndef GAME_EXAMPLE_API_H
#define GAME_EXAMPLE_API_H
/*==============================================================================================

    game_modules/game_example/game_example_api.h — game_example API struct and gateway macro.

==============================================================================================*/

#include "game_modules/game_example/game_example.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct game_example_api_s
{
    void ( *placeholder )( void );

} game_example_api_t;

#if defined( BUILD_STATIC ) || defined( GAME_EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( game_example_api_t, game_example )
#else
MOD_GATEWAY_DYNAMIC( game_example_api_t, game_example )
#endif

#if defined( BUILD_STATIC ) || defined( GAME_EXAMPLE_STATIC )
    #define MOD_USE_GAME_EXAMPLE    /* static build */
    #define MOD_FETCH_GAME_EXAMPLE  true
#else
    #define MOD_USE_GAME_EXAMPLE    MOD_DEFINE_API_PTR( game_example_api_t, game_example )
    #define MOD_FETCH_GAME_EXAMPLE  MOD_FETCH_API( game_example_api_t, game_example )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GAME_EXAMPLE_API_H
