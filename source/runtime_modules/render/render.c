/*==============================================================================================

    render.c — Renderer front-end module (hot-reloadable DLL).

    Sits on top of the RHI. Consumes rhi() for all GPU calls; exposes a
    simple begin/draw/end frame surface to the host. State (clear color, frame
    counter, in-flight command list) persists across reloads.

    Layering
    --------
        rhi (static service)     <- Vulkan backend, swap-chain aware
            ^
            | rhi()->...
            |
        render (this DLL)        <- high-level framing, hot-reloadable
            ^
            | render()->...
            |
        host_main on_update      <- calls begin_frame / draw_frame / end_frame

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_modules/render/render_api.h"

/*==============================================================================================
    Cached API pointers
==============================================================================================*/

MOD_USE_CORE;
MOD_USE_RHI;

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct render_state_s
{
    int   frame_count;
    float total_time;

    float clear_r, clear_g, clear_b, clear_a;

    rhi_command_list_t cmd; /* valid between begin_frame / end_frame; NULL otherwise */

} render_state_t;

static render_state_t* g_state = NULL;

/*==============================================================================================
    API implementations
==============================================================================================*/

static void
render_begin_frame_impl( void )
{
    if ( !g_state )
        return;

    g_state->cmd = rhi()->frame_begin();
    /* NULL means the swap chain is not ready this frame (resize pending, etc.).
       draw_frame and end_frame both guard on cmd != NULL. */
}

static void
render_draw_frame_impl( float dt )
{
    if ( !g_state || !g_state->cmd )
        return;

    g_state->total_time += dt;
    rhi()->cmd_clear_color( g_state->cmd,
                                g_state->clear_r,
                                g_state->clear_g,
                                g_state->clear_b,
                                g_state->clear_a );
}

static void
render_end_frame_impl( void )
{
    if ( !g_state )
        return;

    if ( g_state->cmd )
    {
        rhi()->frame_end();
        g_state->cmd = NULL;
    }

    g_state->frame_count++;
}

static int
render_frame_count_impl( void )
{
    return g_state ? g_state->frame_count : 0;
}

static void
render_set_clear_color_impl( float r, float g, float b, float a )
{
    if ( !g_state )
        return;

    g_state->clear_r = r;
    g_state->clear_g = g;
    g_state->clear_b = b;
    g_state->clear_a = a;
}

/*==============================================================================================
    API struct
==============================================================================================*/

const render_api_t g_render_api_struct = {
    .begin_frame     = render_begin_frame_impl,
    .draw_frame      = render_draw_frame_impl,
    .end_frame       = render_end_frame_impl,
    .frame_count     = render_frame_count_impl,
    .set_clear_color = render_set_clear_color_impl,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
render_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( render_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )
        return false;

    if ( !MOD_FETCH_RHI )
    {
        fprintf( stderr, "[render] failed to fetch rhi_api\n" );
        return false;
    }

    g_state->clear_r = 0.08f;
    g_state->clear_g = 0.10f;
    g_state->clear_b = 0.14f;
    g_state->clear_a = 1.0f;

    // core()->log( "render: init (state=%p)", ( void* )g_state );
    return true;
}

static bool
render_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( render_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )
        return false;

    if ( !MOD_FETCH_RHI )
    {
        fprintf( stderr, "[render] failed to re-fetch rhi_api after reload\n" );
        return false;
    }

    core()->log( "render: reloaded (frames so far = %d)", g_state->frame_count );
    return true;
}

static void
render_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( core() )
        core()->log( "render: exit" );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
render_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( render_state_t ),
        .func_api_size = sizeof( render_api_t ),
        .deps          = { "core", "rhi" },
        .dep_count     = 2,
        .func_api      = &g_render_api_struct,
        .init          = render_init,
        .exit          = render_exit,
        .reload        = render_reload,
    };
    return &api;
}

MOD_DEFINE_EXPORTS( render )

/*============================================================================================*/
