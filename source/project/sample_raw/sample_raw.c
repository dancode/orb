/*==============================================================================================

    sample_raw.c  (compiled as sample_raw.dll)

    The FRAMEWORK-BYPASS project -- proof that the project contract lives at the runtime
    level with no game-framework tether.  This is "not a game": a visualizer-shaped
    project that depends on core + render ONLY and is driven directly by a custom host
    (sb_runtime_project drives the vtable itself; its k_modules[] does not even load the
    game module).  If this target ever needs the game framework to build or run, the
    layering has regressed.

    The scene: a square ping-ponging across the surface, advanced in on_sim at the
    driver's fixed step and drawn interpolated by alpha in on_draw.

==============================================================================================*/

#include "orb.h"
#include "base/math.h"
#define LOG_CH "sample_raw"
#include "engine/mod/mod_export.h"
#include "engine/mod/mod_import.h"

#include "engine/core/core_api.h"
#include "runtime_modules/render/render_api.h"

#include "sample_raw.h"

MOD_USE_CORE;
MOD_USE_RENDER;

/*==============================================================================================
    Persistent state  (zeroed on first load; preserved across hot-reloads)
==============================================================================================*/

#define RAW_SQUARE_SIZE 60.0f
#define RAW_SPEED       0.5f    /* surface widths per second */

typedef struct sample_raw_state_s
{
    bool running;    /* between on_start and on_stop */
    f32  pos;        /* normalized 0..1 across the surface -- survives hot-reload */
    f32  pos_prev;   /* last sim tick's pos -- on_draw lerps prev->cur            */
    f32  dir;        /* +1 / -1 ping-pong direction                               */

} sample_raw_state_t;

static sample_raw_state_t* g_state = NULL;

/*==============================================================================================
    Project contract  (runtime/run_project.h)
==============================================================================================*/

static void
sample_raw_on_start( void )
{
    if ( !g_state )
        return;

    g_state->running  = true;
    g_state->pos_prev = g_state->pos;
    if ( g_state->dir == 0.0f )
        g_state->dir = 1.0f;

    LOG_INFO( "on_start" );
}

static void
sample_raw_on_sim( f32 fixed_dt )
{
    if ( !g_state || !g_state->running )
        return;

    g_state->pos_prev  = g_state->pos;
    g_state->pos      += g_state->dir * RAW_SPEED * fixed_dt;

    if ( g_state->pos > 1.0f ) { g_state->pos = 1.0f; g_state->dir = -1.0f; }
    if ( g_state->pos < 0.0f ) { g_state->pos = 0.0f; g_state->dir =  1.0f; }
}

static void
sample_raw_on_frame( f32 dt, const run_view_t* view )
{
    UNUSED( dt );
    UNUSED( view );
}

static void
sample_raw_on_draw( f32 alpha, const run_view_t* view )
{
    if ( !g_state || !g_state->running )
        return;

    if ( view->render_ctx >= 0 && view->surface_w > 0 && view->surface_h > 0 )
    {
        f32 p      = f32_lerp( g_state->pos_prev, g_state->pos, alpha );
        f32 w      = ( f32 )view->surface_w;
        f32 h      = ( f32 )view->surface_h;
        f32 margin = RAW_SQUARE_SIZE;
        f32 cx     = margin + p * ( w - 2.0f * margin );

        const f32 amber[ 4 ] = { 0.95f, 0.70f, 0.20f, 1.0f };
        render()->submit_rect( view->render_ctx, cx, h * 0.5f,
                               RAW_SQUARE_SIZE, RAW_SQUARE_SIZE, amber );
    }
}

static void
sample_raw_on_stop( void )
{
    if ( !g_state )
        return;

    g_state->running = false;
    LOG_INFO( "on_stop" );
}

const run_project_api_t g_sample_raw_api_struct = {
    .on_start = sample_raw_on_start,
    .on_sim   = sample_raw_on_sim,
    .on_frame = sample_raw_on_frame,
    .on_draw  = sample_raw_on_draw,
    .on_stop  = sample_raw_on_stop,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
sample_raw_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( sample_raw_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )   return false;
    if ( !MOD_FETCH_RENDER ) return false;

    LOG_INFO( "init (core + render only -- no game framework)" );
    return true;
}

static bool
sample_raw_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( sample_raw_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )   return false;
    if ( !MOD_FETCH_RENDER ) return false;

    LOG_INFO( "reloaded (running=%d pos=%.2f)", g_state->running, g_state->pos );
    return true;
}

static void
sample_raw_exit( void* raw_state )
{
    sample_raw_state_t* s = raw_state;
    LOG_INFO( "exit (running=%d)", s ? s->running : 0 );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
sample_raw_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( sample_raw_state_t ),
        .func_api_size = sizeof( run_project_api_t ),
        .deps          = { "core", "render" },
        .dep_count     = 2,
        .func_api      = ( void* )&g_sample_raw_api_struct,
        .init          = sample_raw_init,
        .exit          = sample_raw_exit,
        .reload        = sample_raw_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( sample_raw );

/*============================================================================================*/
