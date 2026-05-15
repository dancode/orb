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

/*============================================================================================*/
#endif    // GAME_H
