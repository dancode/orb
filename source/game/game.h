#ifndef GAME_H
#define GAME_H
/*==============================================================================================

    game.h — Public API exported by the game module.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
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

/*============================================================================================*/
#endif    // GAME_H
