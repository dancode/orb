/*==============================================================================================

    example.c

    Example module — compiled as a hot-reloadable DLL in dynamic builds or
    linked statically in monolithic builds.

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"

#define EXAMPLE_STATIC /* use local struct gateway */

#include "engine/core/core_api.h"
#include "runtime_modules/render/render_api.h"
#include "runtime_modules/example/example_api.h"

/*==============================================================================================
    1. Consumer-side pointer storage for each consumed API (no-op in BUILD_STATIC)
==============================================================================================*/

MOD_DEFINE_API_PTR( core_api_t, core );
MOD_DEFINE_API_PTR( render_api_t, render );

/*==============================================================================================
    2. Persistent state
==============================================================================================*/

typedef struct
{
    bool example_init;
    int  counter;

} example_state_t;

static example_state_t* example_state = NULL;

/*==============================================================================================
    3. API implementations
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
    4. the public API struct, populated at file scope
==============================================================================================*/

const example_api_t g_example_api_struct = {
    .example_function_1 = example_function_1,
    .example_function_2 = example_function_2,
};

/*==============================================================================================
    5. lifecycle callbacks
==============================================================================================*/

static bool
example_init( void* raw_state, get_api_fn get_api )
{
    example_state = ( example_state_t* )raw_state;

    /* cache sibling */
    if ( !MOD_FETCH_API( core_api_t, core ) )
        return false;
    if ( !MOD_FETCH_API( render_api_t, render ) )
        return false;

    /* local api is already available */
    example_api()->example_function_1();

    /* core api is available after fetch in init() */
    example_state->example_init = true;

    core_api()->log( "[example] init: example_state=%d", example_state->example_init );
    return true;
}

static void
example_on_reload( void* raw_state, get_api_fn get_api )
{
    /* same pointer, preserved */
    example_state = ( example_state_t* )raw_state;

    /* re-cache after DLL swap */
    MOD_FETCH_API( core_api_t, core );
    MOD_FETCH_API( render_api_t, render );
}

static void
example_tick( void* raw_state, float dt )
{
    UNUSED( raw_state );
    UNUSED( dt );
}

static void
example_exit( void* raw_state )
{
    /* don't free state */
    UNUSED( raw_state );

    core_api()->log( "[example] exit" );
}

/*==============================================================================================
    6. the lifecycle struct
==============================================================================================*/

mod_api_t*
example_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( example_state_t ),
        .deps       = { "core", "render" },
        .dep_count  = 2,
        .func_api   = &g_example_api_struct,
        .init       = example_init,
        .tick       = example_tick,
        .exit       = example_exit,
        .reload     = example_on_reload,
    };
    return &api;
}

void*
example_get_api( void )
{
    return ( void* )&g_example_api_struct;
}

/*==============================================================================================
    7. DLL export — only present in dynamic builds
==============================================================================================*/

MOD_DEFINE_EXPORTS( example )

/*============================================================================================*/