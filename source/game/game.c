/*==============================================================================================

    game.c : game framework module

    Game framework that uses the engine + runtime.

    This holds the game systems/serices that user games projects are built on top of.

==============================================================================================*/

#include "orb.h"
#include "game/game.h"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"
#include "runtime_modules/audio/audio.h"
#include "runtime_modules/render/render.h"
#include "runtime_modules/physics/physics.h"

MOD_USE_CORE;
MOD_USE_RENDER;
MOD_USE_AUDIO;
MOD_USE_PHYSICS;

/*==============================================================================================
    Game state
==============================================================================================*/

typedef struct game_state_s
{
    int   tick_count;

    int   started;
    int   score;
    float time_in_level;

} game_state_t;

static game_state_t* g_game_state = NULL;

/*==============================================================================================
    Game : Public API
==============================================================================================*/

/*==============================================================================================
    Public API
==============================================================================================*/

static void
game_on_start( void )
{
    if ( !g_game_state )
        return;
    g_game_state->started       = 1;
    g_game_state->score         = 0;
    g_game_state->time_in_level = 0.f;

    // physics()->spawn_body();
    // physics()->spawn_body();
    audio()->play( "intro_jingle", 0.5 );
    core()->log( "game: started" );
}

static void
game_on_update( float dt )
{
    /* would poll input and advance game state here in a real engine */

    UNUSED( dt );
    if ( !g_game_state || !g_game_state->started )
        return;

    g_game_state->time_in_level += dt;

    /* very fake gameplay: every "second" of accumulated time, score +1 */
    if ( g_game_state->time_in_level >= 1.0f )
    {
        g_game_state->score++;
        g_game_state->time_in_level = 0.f;
        core()->log( "game: score = %d", g_game_state->score );
    }
    // physics()->simulate( dt );
}

static void
game_on_render( void )
{
    render()->begin_frame();
    /* would issue draw calls here in a real engine */
    render()->end_frame();
}

static void
game_on_stop( void )
{
    if ( !g_game_state )
        return;

    audio()->stop( 1 );
    g_game_state->started = 0;
    core()->log( "game: stopped (final score = %d)", g_game_state->score );
}

static int
game_score( void )
{
    return g_game_state ? g_game_state->score : 0;
}

/*==============================================================================================
    API struct
==============================================================================================*/

const game_api_t g_game_api_struct = {
    .on_start  = game_on_start,
    .on_update = game_on_update,
    .on_render = game_on_render,
    .on_stop   = game_on_stop,
    .score     = game_score,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
game_init( void* raw_state, get_api_fn get_api )
{
    g_game_state = ( game_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )    return false;
    if ( !MOD_FETCH_RENDER )  return false;
    if ( !MOD_FETCH_AUDIO )   return false;
    if ( !MOD_FETCH_PHYSICS ) return false;

    core()->log( "game: init (deps satisfied)" );
    return true;
}

static bool
game_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_game_state = ( game_state_t* )raw_state;
    MOD_FETCH_CORE;
    MOD_FETCH_RENDER;
    MOD_FETCH_AUDIO;
    MOD_FETCH_PHYSICS;
    core()->log( "game: reloaded (score preserved = %d)", g_game_state->score );
    return true;
}

void
game_exit( void* raw_state )
{
    ( void )raw_state;
    core()->log( "game exit" );
}

// static void
// game_tick( void* raw_state, float dt )
// {
//     UNUSED( raw_state );
//     /* Per-frame work the host doesn't have to know about — we just plumb through. */
//     game_on_update( dt );
//     game_on_render();
// 
// }

/*==============================================================================================
Game module descriptor
==============================================================================================*/

mod_desc_t*
game_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version    = 1,
        .state_size = sizeof( game_state_t ),
        .deps       = { "core", "render", "audio", "physics" },
        .dep_count  = 4,
        .func_api   = &g_game_api_struct,
        .init       = game_init,
        .exit       = game_exit,
        .reload     = game_reload,
    };
    return &api;
}

MOD_DEFINE_EXPORTS( game );

/*============================================================================================*/
