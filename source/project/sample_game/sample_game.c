/*==============================================================================================

    sample_game.c  (compiled as sample_game.dll)

    Example dynamic module.  Demonstrates:
      - pulling core / engine / render APIs through module_sys_api_t
      - persistent state that survives hot-reloads
      - first-load vs reload detection via a sentinel value
      - both required DLL exports: get_mod_desc() and get_api()

    The module system owns all memory for game_state_t.  Never free it inside
    exit() — the system will reuse the same block on the next reload.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/mod/mod.h"

#include "engine/core/core_api.h"
#include "runtime_modules/render/render.h"
#include "game/game.h"

#include "sample_game.h"

MOD_USE_CORE;
MOD_USE_RENDER;
MOD_USE_GAME;

/*============================================================================================*/

// core_api_t*       g_core_api  = NULL;    // Core API pointer
// core_debug_api_t* g_debug_api = NULL;    // Core Debug API pointer

// static int32_t g_draw_debug  = 10;
// static char*   g_player_name = "my_name";

/*==============================================================================================
    Persistent state  (zeroed on first load; preserved across hot-reloads)
==============================================================================================*/

typedef struct example_game_state_s
{
    int   score;
    float timer;

} example_game_state_t;

static example_game_state_t* state = NULL;

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
sample_game_init( void* raw_state, get_api_fn get_api )
{
    example_game_state_t* s = raw_state;
    UNUSED( s );
    UNUSED( get_api );

    // if ( !MOD_FETCH_API( core_api_t, core ) )
    //     return false;
    // if ( !MOD_FETCH_API( render_api_t, render ) )
    //     return false;

    return true;

    // Optionally register cvars
    // core->cvar_register( "r_draw_debug", CVAR_INT, &g_draw_debug );
    // core->cvar_register( "player_name", CVAR_STRING, &g_player_name );s
    // g_api->lsbzog( "[game] binit: r_draw_debug = %d", g_draw_debug );
}

static void
sample_game_exit( void* raw_state )
{
    example_game_state_t* s = raw_state;
    core()->log( "[game] exit — score: %d  timer: %.2fs\n", s->score, s->timer );
}

static bool
sample_game_on_reload( void* raw_state, get_api_fn get_api )
{    
    example_game_state_t* s = raw_state;
    UNUSED( s );
    UNUSED( get_api );

    core()->log( "[game] on_reload — ready\n" );
    return true;
}

/*============================================================================================*/

void
sample_game_function( void )
{
    // printf( "[game] shutdown\n" );

    // g_core_api->log( "game function!!!" );
}

static int
sample_game_get_score( void )
{
    /* In a real module this would reach into the state via a module-level pointer.
       Keeping it simple for the example. */
    return 0;
}

/*==============================================================================================
    This module's own exported API
==============================================================================================*/

typedef struct sample_game_api_s
{
    int ( *get_score )( void );

} sample_game_api_t;

const sample_game_api_t g_sample_game_api_struct = {
    .get_score = sample_game_get_score,
};

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
sample_game_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version    = 1,
        .state_size = sizeof( example_game_state_t ),
        .func_api_size = sizeof( sample_game_api_t ),
        .deps       = { "core", "engine", "render" },
        .dep_count  = 3,
        .func_api   = (void*)&g_sample_game_api_struct,
        .init       = sample_game_init,
        .exit       = sample_game_exit,
        .reload  = sample_game_on_reload,
    };
    return &api;
}

void*
sample_game_get_api( void )
{
    return ( void* )&g_sample_game_api_struct;
}

MOD_DEFINE_EXPORTS( sample_game );

/*============================================================================================*/