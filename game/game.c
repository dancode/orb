/*==============================================================================================

    game.c

    Game framework that uses the engine API. This holds the game systems/serices that user
    projects build on top of. You can think of this as the "game layer" that sits on top of
    the "engine layer". The engine provides the low-level functionality, and the game
    layer provides the high-level.

==============================================================================================*/

#include "orb.h"
#include "core/core.h"
#include "module/module_api.h"
#include "game.h"

static int
game_init( void )
{
    return 0;
}

/*==============================================================================================
    Game module descriptor
==============================================================================================*/

static module_api_t g_module_api = {

    .version    = 1,
    .state_size = 0,

    .deps       = { "core", "engine" },
    .dep_count  = 2,

    .init       = NULL,
    .tick       = NULL,
    .exit       = NULL,
    .on_reload  = NULL,
};

API_EXPORT module_api_t*
get_module_api( void )
{
    return &g_module_api;
}

/*==============================================================================================
    Game module functions
==============================================================================================*/

void
game_function( void )
{
    // printf( "Hello from the game module!\n" );
}

/*==============================================================================================
    Game module API
==============================================================================================*/

static game_api_t g_game_api = {
    .game_function = game_function,
};

API_EXPORT void*
get_api( void )
{
    return &g_game_api;
}

/*============================================================================================*/
