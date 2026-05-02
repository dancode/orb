#ifndef GAME_API_H
#define GAME_API_H

/*==============================================================================================

    game_api.h

    The game module owns the frame.  main() calls module_system_tick() which calls
    game_tick(), which calls renderer_api()->begin_frame() etc.

    main.c never touches Tier 2 module APIs directly.

==============================================================================================*/

#include "module/module_api.h"

typedef struct game_api_s
{
    /* Advance the game by one frame.  Internally calls renderer, audio, etc. */
    void ( *update )( float dt );

} game_api_t;

MODULE_GATEWAY( game_api_t, game )

/*============================================================================================*/
#endif    // GAME_API_H