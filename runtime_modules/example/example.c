/*==============================================================================================

    example.c

    Example module — compiled as a hot-reloadable DLL in dynamic builds,
    linked statically in monolithic builds.

    Depends on: "render"

    CONSUMER PATTERN (using a sibling API):
    ────────────────────────────────────────
    In dynamic builds, each consuming DLL must:
        1. Define pointer storage at file scope   → MODULE_DEFINE_API_PTR
        2. Fetch the pointer in init()            → MODULE_FETCH_API
        3. Re-fetch it in on_reload()             → MODULE_FETCH_API (DLL swap may change address)

    In static builds all three expand to nothing — the struct is linked in directly and
    render_api() returns &g_render_api_struct with zero overhead.

    PROVIDER PATTERN (see notes in render.c):
    ──────────────────────────────────────────
    Same as render.c: define g_render_api_struct as a non-static global const.
        1. Public API exported by the render module → MODULE_DEFINE_ACCESS_FUNC

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "base/orb.h"
#include "module/module_api.h"
#include "runtime/modules/render/render_api.h"
#include "runtime/modules/example/example_api.h"

/*==============================================================================================
    CONSUMER: allocate pointer storage for the example API.
    In static builds this expands to nothing — g_example_api_struct is linked in directly.
    In dynamic builds this defines: const example_api_t* g_example_api_ptr = NULL;
==============================================================================================*/

// MODULE_DEFINE_API_PTR( example_api_t, example );
MODULE_DEFINE_API_PTR( render_api_t, render );

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    bool example_init;
    int  counter;

} example_state_t;

static example_state_t* state = NULL;

/*==============================================================================================
    API implementation
==============================================================================================*/

static void
example_function_1( void )
{
    printf( "[example] function 1\n" );
    return;
}

static void
example_function_2( int value )
{
    printf( "[example] function 2 :%d\n", value );
    return;
}

/*==============================================================================================
    API struct — globally visible, non-static
==============================================================================================*/

const example_api_t g_example_api_struct = {
    .example_function_1 = example_function_1,
    .example_function_2 = example_function_2,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
example_init( void* raw_state /* .exe allocated state */, get_api_fn get_api /* .exe get api func */ )
{
    state = ( example_state_t* )raw_state;
    
    /* CONSUMER: get sibling API.
       Static build: expands to (1) — render_api() resolves at link time, nothing to do.
       Dynamic build: populates g_render_api_ptr; returns false if render is not up. */
    if ( !MODULE_GET_API( render_api_t, render ) )
    {
        printf( "[example] ERROR: render module not available\n" );
        return false;
    }

    printf( "[example] reloaded, s->counter=%d\n", state->counter );


    return true;
}

static void
example_tick( void* raw_state, float dt )
{
    ( void )state;
    ( void )dt;
}

static void
example_exit( void* raw_state )
{
    ( void )state;
    printf( "[example] exit\n" );
}

static void
example_on_reload( void* raw_state, get_api_fn get_api )
{
    /* Re-anchor state and re-fetch any sibling APIs whose DLL may also have been swapped.
       on_reload() is called INSTEAD of init() — state arrives with its previous values. */
    state = ( example_state_t* )raw_state;

    /* In a dynamic build, re-populate g_audio_api_ptr in case audio was also reloaded. */
    MODULE_GET_API( render_api_t, render );
    if ( render_api )
    {
        printf( "[example] reloaded, s->counter=%d\n", state->counter );
    }
}

/*==============================================================================================
    Public getters
==============================================================================================*/

module_api_t*
example_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = sizeof( example_state_t ),
        .deps       = { "render" }, /* render is guaranteed INITIALIZED before example_init() */
        .dep_count  = 1,
        .func_api   = &g_example_api_struct,
        .init       = example_init,
        .tick       = example_tick,
        .exit       = example_exit,
        .on_reload  = example_on_reload,
    };
    return &api;
}

void*
example_get_api( void )
{
    return ( void* )&g_example_api_struct;
}

/* named api exports (dynamic builds only) - library symbol acquired from exe to call into module.
   - static builds export the function struct directly, no pointer needed */

MODULE_DEFINE_EXPORTS( example )

/* expands to:

    __declspec( dllexport ) module_api_t*
    get_module_api( void )
    {
        return example_get_module_api();
    }

    __declspec( dllexport ) void*
    get_api( void )
    {
        return ( void* )example_get_api();
    }

*/

/*============================================================================================*/