/*==============================================================================================

    my_project.c  (compiled as my_project.dll)

    The CANONICAL PROJECT MODULE -- the reference for a game project DLL loaded by a host
    at runtime ( host_game.exe -module my_project, or from a child project directory via
    -project <dir> ), and the source template for `build_tool -create <name> -type project`.

    Demonstrates the full project shape:
      - implements runtime/run_project.h ( run_project_api_t ) as its func_api, so any
        driver can run it generically: on_start / on_sim / on_frame / on_draw / on_stop
      - builds on the game FRAMEWORK module ( game()->on_start / on_update / on_stop --
        the framework's score ticks to the console prove the delegation chain )
      - submits its scene through render() using the rhi context handed in per draw
        ( view->render_ctx; -1 when running headless )
      - persistent state that survives hot-reloads ( edit this file, rebuild, watch the
        swap land without losing position )

    The module system owns all memory for my_project_state_t.  Never free it inside
    exit() -- the system reuses the same block on the next reload.

==============================================================================================*/

#include "orb.h"
#include "base/math.h"
#define LOG_CH "my_project"
#include "engine/mod/mod_export.h"
#include "engine/mod/mod_import.h"

#include "engine/core/core_api.h"
#include "runtime_modules/render/render_api.h"
#include "game/game_api.h"

#include "my_project.h"

MOD_USE_CORE;
MOD_USE_RENDER;
MOD_USE_GAME;

/*==============================================================================================
    Persistent state  (zeroed on first load; preserved across hot-reloads)
==============================================================================================*/

#define SAMPLE_SQUARE_SIZE  80.0f
#define SAMPLE_ORBIT_SPEED  2.5f    /* radians per second -- hot-reload test edit */

typedef struct my_project_state_s
{
    bool running;   /* between on_start and on_stop */
    f32  angle;     /* orbit angle -- proves state survives hot-reload */

} my_project_state_t;

static my_project_state_t* g_state = NULL;

/*==============================================================================================
    Project contract  (runtime/run_project.h)
==============================================================================================*/

static void
my_project_on_start( void )
{
    if ( !g_state )
        return;

    g_state->running = true;
    game()->on_start();    /* framework: resets score, starts the fake gameplay loop */
    LOG_INFO( "on_start" );
}

static void
my_project_on_sim( f32 fixed_dt )
{
    if ( !g_state || !g_state->running )
        return;

    game()->on_update( fixed_dt );    /* framework score ticks to the console every second */

    g_state->angle += SAMPLE_ORBIT_SPEED * fixed_dt;
}

static void
my_project_on_frame( f32 dt, const run_view_t* view )
{
    /* Per-frame, framerate-bound work (cameras, effects) goes here -- none yet. */
    UNUSED( dt );
    UNUSED( view );
}

static void
my_project_on_draw( f32 alpha, const run_view_t* view )
{
    UNUSED( alpha );    /* no prev-state lerp yet -- draws the latest sim state */

    if ( !g_state || !g_state->running )
        return;

    /* Scene submission -- a square orbiting the surface center.  render()->draw_scene
       replays this behind whatever the host composites on top (editor gui, HUD).  The
       surface size comes from the view so the project never touches rhi directly. */
    if ( view->render_ctx >= 0 && view->surface_w > 0 && view->surface_h > 0 )
    {
        f32 w  = ( f32 )view->surface_w;
        f32 h  = ( f32 )view->surface_h;
        f32 cx = w * 0.5f + f32_cos( g_state->angle ) * w * 0.25f;
        f32 cy = h * 0.5f + f32_sin( g_state->angle ) * h * 0.25f;

        const f32 teal[ 4 ] = { 0.20f, 0.80f, 0.70f, 1.0f };
        render()->submit_rect( view->render_ctx, cx, cy,
                               SAMPLE_SQUARE_SIZE, SAMPLE_SQUARE_SIZE, teal );
    }
}

static void
my_project_on_stop( void )
{
    if ( !g_state )
        return;

    g_state->running = false;
    game()->on_stop();    /* framework: logs the final score */
    LOG_INFO( "on_stop" );
}

const run_project_api_t g_my_project_api_struct = {
    .on_start = my_project_on_start,
    .on_sim   = my_project_on_sim,
    .on_frame = my_project_on_frame,
    .on_draw  = my_project_on_draw,
    .on_stop  = my_project_on_stop,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
my_project_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( my_project_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )   return false;
    if ( !MOD_FETCH_RENDER ) return false;
    if ( !MOD_FETCH_GAME )   return false;

    LOG_INFO( "init (deps satisfied)" );
    return true;
}

static bool
my_project_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( my_project_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )   return false;
    if ( !MOD_FETCH_RENDER ) return false;
    if ( !MOD_FETCH_GAME )   return false;

    LOG_INFO( "reloaded (running=%d angle=%.2f)", g_state->running, g_state->angle );
    return true;
}

static void
my_project_exit( void* raw_state )
{
    my_project_state_t* s = raw_state;
    LOG_INFO( "exit (running=%d)", s ? s->running : 0 );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
my_project_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( my_project_state_t ),
        .func_api_size = sizeof( run_project_api_t ),
        .deps          = { "core", "game", "render" },
        .dep_count     = 3,
        .func_api      = ( void* )&g_my_project_api_struct,
        .init          = my_project_init,
        .exit          = my_project_exit,
        .reload        = my_project_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( my_project );

/*============================================================================================*/
