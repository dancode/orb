/*==============================================================================================

    render.c

    Renderer module.

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"

#include "render_api.h"

MOD_DEFINE_API_PTR( core_api_t, core );

/*============================================================================================*/

typedef struct render_state_s
{
    int   frame_count;
    float total_time;

    int   total_draw_calls;
    int   frame_draw_calls;
    bool  in_frame;

} render_state_t;

static render_state_t* g_state = NULL;

/*==============================================================================================
    Implementation — shared by both build modes
==============================================================================================*/

static void
render_begin_frame_impl( void )
{
    if ( !g_state )
        return;
    /* core_api() is the right call here — works in both static and dynamic builds */
    core_api()->log( "render: begin frame %d", g_state->frame_count );
}

static void
render_draw_frame_impl( float dt )
{
    if ( !g_state )
        return;
    g_state->total_time += dt;
}

static void
render_end_frame_impl( void )
{
    if ( !g_state )
        return;
    g_state->frame_count++;
}

static int
render_frame_count_impl( void )
{
    return g_state ? g_state->frame_count : 0;
}

static void
render_set_clear_color_impl( float r, float g, float b )
{
    if ( !g_state )
        return;

    UNUSED( r );
    UNUSED( g );
    UNUSED( b );
}

/*==============================================================================================
    API struct — globally visible, non-static (see audio.c for naming rationale).
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
    g_state = ( render_state_t* )raw_state;

    if ( !MOD_FETCH_API( core_api_t, core ) )
        return false;

    core_api()->log( "render: init (state=%p)", ( void* )g_state );
    return true;
}

void
render_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( core_api() )
        core_api()->log( "render: exit" );
}

static bool
render_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );

    g_state = ( render_state_t* )raw_state;
    MOD_FETCH_API( core_api_t, core );

    core_api()->log( "render: reloaded (frames so far = %d)", g_state->frame_count );
    return true;
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
render_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( render_state_t ),
        .func_api_size = sizeof( render_api_t ),
        .deps       = { "core" },    // "app" + remove "engine"
        .dep_count  = 1,
        .func_api   = &g_render_api_struct,
        .init       = render_init,
        .exit       = render_exit,
        .reload     = render_reload,
    };
    return &api;
}

void*
render_get_api( void )
{
    return ( void* )&g_render_api_struct;
}

MOD_DEFINE_EXPORTS( render );

/*============================================================================================*/
