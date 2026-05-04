/*==============================================================================================

    render.c

    Renderer module.

==============================================================================================*/

#include <stdio.h>
#include "base/orb.h"

#define RENDER_LINK_STATIC /* use local struct gateway */
#include "module/module_api.h"
#include "render_api.h"

#include "core/core_api.h"
#include "engine_api.h"


MODULE_DEFINE_API_PTR( core_api_t, core );
MODULE_DEFINE_API_PTR( engine_api_t, engine );
// MODULE_DEFINE_API_PTR( render_api_t, render ); 

/*============================================================================================*/

typedef struct render_state_s
{
    int  frame_count;
    int  total_frames;
    int  total_draw_calls;
    int  frame_draw_calls;
    bool in_frame;

} render_state_t;

static render_state_t* state = NULL;

/*==============================================================================================
    Implementation — shared by both build modes
==============================================================================================*/

static void
render_impl_draw_frame( float dt )
{
    // printf( "drawing frame with time: %f\n", dt );
}

static void
render_impl_set_clear_color( float r, float g, float b )
{
    printf( "set clear color\n" );
}

/*==============================================================================================
    API struct — globally visible, non-static (see audio.c for naming rationale).
==============================================================================================*/

const render_api_t g_render_api_struct = {
    .draw_frame      = render_impl_draw_frame,
    .set_clear_color = render_impl_set_clear_color,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
render_init( void* raw_state, get_api_fn get_api )
{
    /* CONSUMER: fetch sibling API.
       Static build: expands to (1) — audio_api() resolves at link time, nothing to do.
       Dynamic build: populates g_audio_api_ptr; returns false if audio is not up. */

    state = ( render_state_t* )raw_state;

    /* In a dynamic build, re-populate g_audio_api_ptr in case audio was also reloaded. */

    if ( !MODULE_GET_API( core_api_t, core ) )
    {
        return false;
    }

    render_api()->set_clear_color( 0.1f, 0.2f, 0.3f );

    // g_engine = ( const engine_api_t* )get_api( "engine" );

    return true;
}

void
render_exit( void* raw_state )
{
    ( void )state;
    printf( "[renderer] exit\n" );
}

void
render_tick( void* raw_state, float dt )
{
    // printf( "[game] tick\n" );

    render_state_t* s = (render_state_t*)raw_state;
    s->frame_count++;
}

static void
render_on_reload( void* raw_state, get_api_fn get_api )
{
    /* Re-anchor state and re-fetch any sibling APIs whose DLL may also have been swapped.
       on_reload() is called INSTEAD of init() — state arrives with its previous values. */

    state = ( render_state_t* )raw_state;

    /* In a dynamic build, re-populate g_audio_api_ptr in case audio was also reloaded. */
    MODULE_GET_API( core_api_t, core );
    // core_api()->log( "[renderer] on_reload: state preserved, frame_count=%d\n", state->frame_count );

    printf( "[renderer] reloaded  frames=%d  total_draws=%d\n", state->total_frames, state->total_draw_calls );

}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

module_api_t*
render_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = sizeof( render_state_t ),
        .deps       = { "core", "engine" }, // "app" + remove "engine"
        .dep_count  = 2,
        .func_api   = &g_render_api_struct,
        .init       = render_init,
        .tick       = render_tick,
        .exit       = render_exit,
        .on_reload  = render_on_reload,
    };
    return &api;
}

void*
render_get_api( void )
{
    return ( void* )&g_render_api_struct;
}

MODULE_DEFINE_EXPORTS( render );

/*============================================================================================*/
