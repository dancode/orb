/*==============================================================================================

    render.c

==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "render.h"

#include "module/module_sys_api.h" /* module_sys_api_t                  */
#include "module/module_api.h"     /* module_api_t, mod_init_fn, etc.  */
#include "core/core_api.h"         /* core_api_t                        */
#include "engine_api.h"            /* engine_api_t                      */

/*============================================================================================*/

#define STATE_SENTINEL 0xC0FFEE /* written on first init; lets us detect reloads */

typedef struct render_state_s
{
    uint32_t sentinel; /* first-load detection                           */

    /* refreshed in init() — may differ after reload  */

    core_api_t*   core;
    engine_api_t* engine;
    render_api_t* render;

    /* gameplay data — survives hot-reloads */

    int frame_count;

} render_state_t;

/*==============================================================================================
    Lifecycle
==============================================================================================*/

bool
render_init( void* raw_state, module_sys_api_t* sys )
{
    // printf( "[game] init\n" );

    render_state_t* s = raw_state;

    /* Pull APIs from the registry every init() — pointers may have changed
       if a dependency was also reloaded. */

    s->core   = sys->get_api( "core" );
    s->engine = sys->get_api( "engine" );

    if ( !s->core || !s->engine )
    {
        /* Can't log without core — fall back to printf. */
        // printf( "[game] init failed: missing dependency\n" );
        return false;
    }

    if ( s->sentinel != STATE_SENTINEL )
    {
        /* First load — state was zero-filled by the module system. */
        s->sentinel    = STATE_SENTINEL;
        s->frame_count = 0;
        s->core->log( "[render] first load - fresh state" );
    }
    else
    {
        /* Hot-reload — gameplay data is intact. */
        s->core->log( "[render] reloaded — frame_count %d preserved", s->frame_count );
    }

    return true;
}

void
render_tick( void* raw_state, float dt )
{
    // printf( "[game] tick\n" );

    render_state_t* s = raw_state;
    s->frame_count++;
}

void
render_exit( void* raw_state )
{
    // printf( "[game] shutdown\n" );

    render_state_t* s = raw_state;
    /* State is NOT freed here — the system owns it.
       Just flush anything that needs flushing. */
    s->core->log( "[render] exit — frame_count: %d\n", s->frame_count );
}

static void
render_on_reload( void* raw_state, module_sys_api_t* sys )
{
    /* init() already re-cached all API pointers.
       Use on_reload for anything that only makes sense after a code swap —
       e.g. re-registering callbacks, resetting renderer state, etc. */
    render_state_t* s = raw_state;
    ( void )sys;
    s->core->log( "[render] on_reload — ready\n" );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

static module_api_t g_module_api = {
    .version    = 1,
    .state_size = sizeof( render_state_t ),

    .deps       = { "core", "engine" },
    .dep_count  = 2,

    .init       = render_init,
    .tick       = render_tick,
    .exit       = render_exit,
    .on_reload  = render_on_reload,
};

/*==============================================================================================
    Render API
==============================================================================================*/

// static int
// render_get_framecount( void )
// {
//     /* In a real module this would reach into the state via a module-level pointer.
//        Keeping it simple for the example. */
//     return 0;
// }
// 
// void
// render_print( const char* msg )
// {
//     printf( msg );
// }
// 
// float
// render_add( float a, float b )
// {
//     return a + b;
// }

/*==============================================================================================
    Implementation — shared by both build modes
==============================================================================================*/

static void
render_impl_draw_frame( float dt )
{
    printf( "drawing frame with time: %f\n", dt  );
}

static void
render_impl_set_clear_color( float r, float g, float b )
{ 
    printf( "set clear color\n" );
}

static const render_api_t g_render_api = {
    .draw_frame      = render_impl_draw_frame,
    .set_clear_color = render_impl_set_clear_color,
};

const render_api_t*
get_render_api( void )
{
    return &g_render_api;
}

/*==============================================================================================
    Required DLL exports (C linkage)
==============================================================================================*/

API_EXPORT module_api_t*
get_module_api( void )
{
    return &g_module_api;
}

API_EXPORT const void*
get_api( void )
{
    return &g_render_api;
}

/* game code */
// const renderer_api_t* r = sys->get_api( "renderer" );
// r->draw_frame( dt );

/*==============================================================================================
    Static build — compile-time const table
    The compiler sees concrete function addresses.
    LTO can inline across the pointer call if it chooses to.
==============================================================================================*/

// static render_api_t g_render_api = {
//     .get_framecount = render_get_framecount,
//     .render_print   = render_print,
//     .add            = render_add,
// };

/*============================================================================================*/
