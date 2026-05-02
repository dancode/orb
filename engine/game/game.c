/*==============================================================================================

    game.c

    Game framework that uses the engine API. This holds the game systems/serices that user
    projects build on top of. You can think of this as the "game layer" that sits on top of
    the "engine layer". The engine provides the low-level functionality, and the game
    layer provides the high-level.

==============================================================================================*/

#include "orb.h"
#include "module/module_api.h"

#include "core/core_api.h"
#include "engine_api.h"
#include "game_api.h"

#include "runtime/modules/render/render_api.h"

/* the modules we depend on — we fetch their API pointers in init() and on_reload() */

MODULE_DEFINE_API_PTR( core_api_t, core );
MODULE_DEFINE_API_PTR( engine_api_t, engine );
MODULE_DEFINE_API_PTR( render_api_t, render );

/*==============================================================================================
    Game state
==============================================================================================*/

typedef struct game_state_s
{
    int tick_count;

} game_state_t;

static game_state_t* state = NULL;

/*==============================================================================================
    Game module functions
==============================================================================================*/

static void
game_update( float dt )
{
    UNUSED( dt );

    /* Tier 2 APIs consumed here, inside a module that properly fetched them. */

    // render_api()->begin_frame();
    // render_api()->draw_quad( 0.0f, 0.0f, 800.0f, 600.0f, 0x181818FF );
    // render_api()->end_frame();
    // audio_api()->play( "ambient.ogg", 0.4f );

    /* spammy */
    // core_api()->log( "game tick %d", state->tick_count++ );
    
}

/*==============================================================================================
    API struct
==============================================================================================*/

const game_api_t g_game_api_struct = {
    .update = game_update,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
game_init( void* raw_state, get_api_fn get_api )
{
    state = ( game_state_t* )raw_state;
    MODULE_GET_API( core_api_t, core );
    MODULE_GET_API( engine_api_t, engine );
    MODULE_GET_API( render_api_t, render );
    core_api()->log( "game init" );
    return true;
}

void
game_exit( void* raw_state )
{
    ( void )raw_state;
    core_api()->log( "game exit" );
}

static void
game_tick( void* raw_state, float dt )
{
    ( void )raw_state;
    game_update( dt );
}

static void
game_on_reload( void* raw_state, get_api_fn get_api )
{
    state = ( game_state_t* )raw_state;

    MODULE_GET_API( core_api_t, core );
    MODULE_GET_API( engine_api_t, engine );
    MODULE_GET_API( render_api_t, render );

    core_api()->log( "game reloaded  tick=%d", state->tick_count );
}

/*==============================================================================================
Game module descriptor
==============================================================================================*/

module_api_t*
game_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = sizeof( game_state_t ),
        .deps       = { "core", "engine", "render" },
        .dep_count  = 3,
        .func_api   = &g_game_api_struct,
        .init       = game_init,
        .tick       = game_tick,
        .exit       = game_exit,
        .on_reload  = game_on_reload,
    };
    return &api;
}

void*
game_get_api( void )
{
    return ( void* )&g_game_api_struct;
}

MODULE_DEFINE_EXPORTS( game );

/*============================================================================================*/
