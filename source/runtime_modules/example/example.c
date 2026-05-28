/*==============================================================================================

    example.c : example module implementation, compiled as a DLL in dynamic builds.

     This file is compiled as a DLL in dynamic builds, and linked into the exe in static builds.
     The BUILD_STATIC flag controls which.  See build commands below.
     The module system loads this at runtime and calls its lifecycle callbacks.  It can also
     hot-reload it when the file changes on disk.

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"

#include "engine/mod/mod_export.h"
#include "runtime_modules/example/example_api.h"

/*==============================================================================================
    1. Consumer-side pointer storage for each consumed API (no-op in BUILD_STATIC)
==============================================================================================*/

// MOD_DEFINE_API_PTR( core_api_t, core );

/*==============================================================================================
    2. Persistent state
==============================================================================================*/

typedef struct
{
    bool example_init;
    int  reload_count;
    int  counter;
    bool fail_next_reload;

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

static void
example_fail_next_reload( void )
{
    if ( example_state )
    {
        example_state->fail_next_reload = true;
        printf( "[example] fail_next_reload armed - next on_reload will return false\n" );
    }
}

static void
example_update( float dt )
{
    UNUSED( dt );
    example_state->counter++;
    if ( example_state->counter % 60 == 0 )
        printf( "[example] update: counter=%d\n", example_state->counter );
}

/*==============================================================================================
    4. the public API struct, populated at file scope
==============================================================================================*/

const example_api_t g_example_api_struct = {
    .example_function_1 = example_function_1,
    .example_function_2 = example_function_2,
    .fail_next_reload   = example_fail_next_reload,
    .update             = example_update
};

/*==============================================================================================
    5. lifecycle callbacks
==============================================================================================*/

static bool
example_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    example_state = ( example_state_t* )raw_state;

    /* cache sibling */
    // if ( !MOD_FETCH_API( core_api_t, core ) )
    //     return false;

    /* local api is already available */
    example()->example_function_1();

    /* core api is available after fetch in init() */
    example_state->example_init = true;

    printf( "[example] init: example_state=%d\n", example_state->example_init );
    return true;
}

static bool
example_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    /* same pointer, preserved */
    example_state = ( example_state_t* )raw_state;

    if ( example_state->fail_next_reload )
    {
        example_state->fail_next_reload = false; /* one-shot — snapshot_rollback path will succeed */
        printf( "[example] on_reload: simulating failure (returning false)\n" );
        return false;
    }

    example_state->reload_count++;
    printf( "[example] on_reload: reload_count=%d\n", example_state->reload_count );

    if ( example_state->counter > 0 )
        printf( "[example] on_reload: counter=%d\n", example_state->counter );

    printf( "\n\n VISUAL STUDIO CODE!!!! \n\n" );

    /* re-cache after DLL swap */
    // MOD_FETCH_API( core_api_t, core );

    return true;
}

static void
example_mod_exit( void* raw_state )
{
    /* don't free state */
    UNUSED( raw_state );
    printf( "[example] exit: example_state=%d\n", example_state->example_init );
    printf( "[example] exit: counter=%d\n", example_state->counter );
}

/*==============================================================================================
    6. the lifecycle struct
==============================================================================================*/

mod_desc_t*
example_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( example_state_t ),
        .func_api_size = sizeof( example_api_t ),
        .dep_count     = 0,
        .deps          = { NULL },
        .func_api      = &g_example_api_struct,
        .init          = example_mod_init,
        .exit          = example_mod_exit,
        .reload        = example_mod_reload,
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