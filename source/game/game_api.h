#ifndef GAME_API_H
#define GAME_API_H
/*==============================================================================================

    game/game_api.h — game module API struct and gateway macro.

==============================================================================================*/

#include "game/game.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct game_api_s
{
    void ( *on_start )( void );
    void ( *on_update )( float dt );
    void ( *on_render )( void );
    void ( *on_stop )( void );
    int  ( *score )( void );

} game_api_t;

#if defined( BUILD_STATIC ) || defined( GAME_STATIC )
MOD_GATEWAY_STATIC( game_api_t, game )
#else
MOD_GATEWAY_DYNAMIC( game_api_t, game )
#endif

#if defined( BUILD_STATIC ) || defined( GAME_STATIC )
    #define MOD_USE_GAME    /* static build */
    #define MOD_FETCH_GAME  true
#else
    #define MOD_USE_GAME    MOD_DEFINE_API_PTR( game_api_t, game )
    #define MOD_FETCH_GAME  MOD_FETCH_API( game_api_t, game )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GAME_API_H
