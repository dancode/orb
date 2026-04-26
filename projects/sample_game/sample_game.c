/*==============================================================================================

    sample_game.c  (compiled as sample_game.dll)

    Example dynamic module.  Demonstrates:
      - pulling core / engine / render APIs through module_sys_api_t
      - persistent state that survives hot-reloads
      - first-load vs reload detection via a sentinel value
      - both required DLL exports: get_module_api() and get_api()

    The module system owns all memory for game_state_t.  Never free it inside
    exit() — the system will reuse the same block on the next reload.

==============================================================================================*/

#include "orb.h"
#include "module/module_api.h"     /* module_api_t, mod_init_fn, etc.  */
#include "core/core_api.h"         /* core_api_t                        */
#include "engine_api.h"            /* engine_api_t                      */
#include "systems/render/render.h" /* render_api_t  (example)           */

#include "sample_game.h"
// #include "game_api.h"         /* game_api_t    (this module's API) */

/*==============================================================================================
    Persistent state  (zeroed on first load; preserved across hot-reloads)
==============================================================================================*/

#define STATE_SENTINEL 0xC0FFEE /* written on first init; lets us detect reloads */

typedef struct
{
    uint32_t sentinel; /* first-load detection                           */

    /* refreshed in init() — may differ after reload  */

    core_api_t*   core;
    engine_api_t* engine;
    render_api_t* render;

    /* gameplay data — survives hot-reloads */

    int   score;
    float timer;

} game_state_t;

// core_api_t*       g_core_api  = NULL;    // Core API pointer
// core_debug_api_t* g_debug_api = NULL;    // Core Debug API pointer

// static int32_t g_draw_debug  = 10;
// static char*   g_player_name = "my_name";

/*==============================================================================================
    Lifecycle
==============================================================================================*/

bool
game_init( void* raw_state, module_sys_api_t* sys )
{
    // printf( "[game] init\n" );

    game_state_t* s = raw_state;

    /* Pull APIs from the registry every init() — pointers may have changed
       if a dependency was also reloaded. */
    s->core   = sys->get_api( "core" );
    s->engine = sys->get_api( "engine" );
    s->render = sys->get_api( "render" );

    if ( !s->core || !s->engine || !s->render )
    {
        /* Can't log without core — fall back to printf. */
        // printf( "[game] init failed: missing dependency\n" );
        return false;
    }

    if ( s->sentinel != STATE_SENTINEL )
    {
        /* First load — state was zero-filled by the module system. */
        s->sentinel = STATE_SENTINEL;
        s->score    = 0;
        s->timer    = 0.0f;
        s->core->log( "[game] first load - fresh state" );
    }
    else
    {
        /* Hot-reload — gameplay data is intact. */
        s->core->log( "[game] reloaded — score %d, timer %.2fs preserved", s->score, s->timer );
    }

    return true;

    // Optionally register cvars
    // core->cvar_register( "r_draw_debug", CVAR_INT, &g_draw_debug );
    // core->cvar_register( "player_name", CVAR_STRING, &g_player_name );s
    // g_api->lsbzog( "[game] binit: r_draw_debug = %d", g_draw_debug );
}

void
game_tick( void* raw_state, float dt )
{
    // printf( "[game] tick\n" );

    game_state_t* s = raw_state;
    s->timer += dt;
    // s->render->draw_frame( dt );
    s->render->draw_frame( 99.0f );

    // if ( g_draw_debug )
    // {
    //     g_core_api->log( "[game] drawing debug stuff" );
    // }
}

void
game_exit( void* raw_state )
{
    // printf( "[game] shutdown\n" );

    game_state_t* s = raw_state;
    /* State is NOT freed here — the system owns it.
       Just flush anything that needs flushing. */
    s->core->log( "[game] exit — score: %d  timer: %.2fs\n", s->score, s->timer );

    // g_core_api->log( "[game] exit — final score: %d\n", s->score );

    // g_core_api->log( "[game] shutdown" );
}

static void
game_on_reload( void* raw_state, module_sys_api_t* sys )
{
    /* init() already re-cached all API pointers.
       Use on_reload for anything that only makes sense after a code swap —
       e.g. re-registering callbacks, resetting renderer state, etc. */
    game_state_t* s = raw_state;
    ( void )sys;
    s->core->log( "[game] on_reload — ready\n" );
}

/*============================================================================================*/

void
game_function( void )
{
    // printf( "[game] shutdown\n" );

    // g_core_api->log( "game function!!!" );
}

/*==============================================================================================
    This module's own exported API
==============================================================================================*/

static int
game_get_score( void )
{
    /* In a real module this would reach into the state via a module-level pointer.
       Keeping it simple for the example. */
    return 0;
}

typedef struct game_api_s
{
    int ( *get_score )( void );

} game_api_t;

static game_api_t g_game_api = {
    .get_score = game_get_score,
};

/*==============================================================================================
    Module descriptor
==============================================================================================*/

static module_api_t g_module_api = {
    .version    = 1,
    .state_size = sizeof( game_state_t ),

    .deps       = { "core", "engine", "render" },
    .dep_count  = 3,

    .init       = game_init,
    .tick       = game_tick,
    .exit       = game_exit,
    .on_reload  = game_on_reload,
};

/*==============================================================================================
    Required DLL exports (C linkage)
==============================================================================================*/

API_EXPORT module_api_t*
get_module_api( void )
{
    return &g_module_api;
}

API_EXPORT void*
get_api( void )
{
    return &g_game_api;
}

/*============================================================================================*/