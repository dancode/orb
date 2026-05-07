/*==============================================================================================

    app_module.c

    Wraps app_api_t as a static service.

    app depends on core (for log/alloc) and on sys
    directly for its OS calls (sys is statically linked,
    never registered in the module system).

    It does NOT depend on the module system's "sys" entry
    because sys is below the module system — it is linked in,
    not looked up at runtime.

==============================================================================================*/

#include "orb.h"
#include "engine/app/app_api.h"
#include "engine/mod/mod_api.h"

#include "engine/core/core_api.h"
MOD_DEFINE_API_PTR( core_api_t, core );


/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    int window_count;

} app_state_t;

static app_state_t* s = NULL;

/*==============================================================================================
    API implementation
==============================================================================================*/

void
app_function( void )
{
    /* example function to export in the API struct */
}

const app_api_t g_app_api_struct = { .app_function = app_function };

/*==============================================================================================
    Lifecycle
==============================================================================================*/


static bool
app_init( void* raw_state, get_api_fn get_api )
{
    app_state_t* state = raw_state;
    UNUSED( state );
    MOD_FETCH_API( core_api_t, core );    

    if ( core_api() == NULL )
        return false;

    /* sys calls are made directly here — no lookup needed */
    core_api()->log( "[app] init\n" );
    // return app_open_window( "My Game", 1280, 720 );
    return true;
}

static void
app_tick( void* raw_state, float dt )
{
    ( void )raw_state;
    ( void )dt;
    // app_poll_events(); /* sys call — direct */
}

static void
app_exit( void* raw_state )
{
    app_state_t* state = raw_state;
    UNUSED( state );
    core_api()->log( "[app] exit\n" );
    // app_close_window();
}

/*============================================================================================*/

mod_api_t*
app_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( app_state_t ),
        .deps       = { "core" }, /* needs core for log/alloc, nothing else */
        .dep_count  = 1,

        .init       = app_init,
        .tick       = app_tick,
        .exit       = app_exit,
        .reload     = NULL,
    };
    return &api;
}

void*
app_get_api( void )
{
    return ( void* )&g_app_api_struct;
}

/*============================================================================================*/