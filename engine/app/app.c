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
#include "base/base.h"
#include "core/core_api.h"
#include "app/app_api.h"
#include "module/module_api.h"
// #include "module_sys/module_sys.h"

const core_api_t* core = NULL;

typedef struct
{
    int window_count;

} app_state_t;

static bool
app_init( void* raw_state, get_api_fn get_api )
{
    app_state_t* s = raw_state;

    core = (core_api_t*)get_api( "core" ); /* safe — core already initialized */
    if ( core == NULL )
        return false;

    /* sys calls are made directly here — no lookup needed */
    core->log( "[app] init\n" );
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
    app_state_t* s = raw_state;
    core->log( "[app] exit\n" );
    // app_close_window();
}

/*============================================================================================*/

static module_api_t g_module_api = {
    .version    = 1,
    .state_size = sizeof( app_state_t ),
    .deps       = { "core" }, /* needs core for log/alloc, nothing else */
    .dep_count  = 1,

    .init       = app_init,
    .tick       = app_tick,
    .exit       = app_exit,
    .on_reload  = NULL,
};

/*============================================================================================*/

typedef struct app_api_s
{
    void ( *app_function )( void );
} app_api_t;

void
app_function( void )
{
    /* example function to export in the API struct */
}

static app_api_t g_api = 
{
    .app_function = app_function
};

/*============================================================================================*/

// module_api_t*
// app_get_module_api( void )
// {
//     return &g_module_api;
// }
// 
// app_api_t*
// app_get_api( void )
// {
//     return &g_api;
// }

// void
// app_module_register( void )
// {
//     // module_register_static( "app", &g_app_module_api, app_get_api() );
//     // service_register( "app", &g_app_module_api, app_get_api() );
// }

/*============================================================================================*/