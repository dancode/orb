/*==============================================================================================

    game.c : game framework module

    Game framework that uses the engine + runtime.

    This holds the game systems/serices that user games projects are built on top of.

==============================================================================================*/

#include "orb.h"
#include "game/game_api.h"

#include "engine/mod/mod_export.h"
#include "engine/core/core.h"
#include "runtime_module/audio/audio_api.h"
#include "runtime_module/render/render_api.h"
#include "runtime_module/physics/physics_api.h"

MOD_DEFINE_API_PTR( core_api_t, core );
MOD_DEFINE_API_PTR( render_api_t, render );
MOD_DEFINE_API_PTR( audio_api_t, audio );
MOD_DEFINE_API_PTR( physics_api_t, physics );

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

    // physics_api()->spawn_body();
    // physics_api()->spawn_body();
    audio_api()->play( "intro_jingle", 0.5 );
    core_api()->log( "game: started" );
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
        core_api()->log( "game: score = %d", g_game_state->score );
    }
    // physics_api()->simulate( dt );
}

static void
game_on_render( void )
{
    render_api()->begin_frame();
    /* would issue draw calls here in a real engine */
    render_api()->end_frame();
}

static void
game_on_stop( void )
{
    if ( !g_game_state )
        return;

    audio_api()->stop( 1 );
    g_game_state->started = 0;
    core_api()->log( "game: stopped (final score = %d)", g_game_state->score );
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

    if ( !MOD_FETCH_API( core_api_t, core ) )
        return false;
    if ( !MOD_FETCH_API( render_api_t, render ) )
        return false;
    if ( !MOD_FETCH_API( audio_api_t, audio ) )
        return false;
    if ( !MOD_FETCH_API( physics_api_t, physics ) )
        return false;

    core_api()->log( "game: init (deps satisfied)" );
    return true;
}

static bool
game_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_game_state = ( game_state_t* )raw_state;
    MOD_FETCH_API( core_api_t, core );
    MOD_FETCH_API( render_api_t, render );
    MOD_FETCH_API( audio_api_t, audio );
    MOD_FETCH_API( physics_api_t, physics );
    core_api()->log( "game: reloaded (score preserved = %d)", g_game_state->score );
    return true;
}

void
game_exit( void* raw_state )
{
    ( void )raw_state;
    core_api()->log( "game exit" );
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

mod_api_t*
game_get_mod_api( void )
{
    static mod_api_t api = {
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
