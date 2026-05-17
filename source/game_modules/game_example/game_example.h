#ifndef GAME_EXAMPLE_H
#define GAME_EXAMPLE_H
/*==============================================================================================

    game_modules/game_example/game_example.h — Example game module API.

    Stub module demonstrating the game module pattern.
    Hot-reloadable in dynamic builds; statically linked in monolithic builds.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct game_example_api_s
{
    void ( *placeholder )( void );

} game_example_api_t;

#if defined( BUILD_STATIC ) || defined( GAME_EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( game_example_api_t, game_example )
mod_desc_t* game_example_get_mod_desc( void );
#else
MOD_GATEWAY_DYNAMIC( game_example_api_t, game_example )
#endif

/*============================================================================================*/
#endif    // GAME_EXAMPLE_H
