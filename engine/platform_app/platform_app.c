/*==============================================================================================

    platform_app_module.c

    Wraps platform_app_api_t as a static service.

    platform_app depends on core (for log/alloc) and on platform_sys
    directly for its OS calls (platform_sys is statically linked,
    never registered in the module system).

    It does NOT depend on the module system's "platform_sys" entry
    because platform_sys is below the module system — it is linked in,
    not looked up at runtime.

==============================================================================================*/

#include "orb.h"
#include "base/base.h"
#include "core/core_api.h"
#include "platform_app_api.h"
#include "module/module_api.h"
// #include "module_sys/module_sys.h"

typedef struct
{
    core_api_t* core;

} platform_app_state_t;

static bool
platform_app_init( void* raw_state, module_sys_api_t* sys )
{
    platform_app_state_t* s = raw_state;

    s->core                 = sys->get_api( "core" ); /* safe — core already initialized */
    if ( !s->core )
        return false;

    /* platform_sys calls are made directly here — no lookup needed */
    s->core->log( "[platform_app] init\n" );
    // return platform_app_open_window( "My Game", 1280, 720 );
    return true;
}

static void
platform_app_tick( void* raw_state, float dt )
{
    ( void )raw_state;
    ( void )dt;
    // platform_app_poll_events(); /* platform_sys call — direct */
}

static void
platform_app_exit( void* raw_state )
{
    platform_app_state_t* s = raw_state;
    s->core->log( "[platform_app] exit\n" );
    // platform_app_close_window();
}

/*============================================================================================*/

static module_api_t g_module_api = {
    .version    = 1,
    .state_size = sizeof( platform_app_state_t ),
    .deps       = { "core" }, /* needs core for log/alloc, nothing else */
    .dep_count  = 1,

    .init       = platform_app_init,
    .tick       = platform_app_tick,
    .exit       = platform_app_exit,
    .on_reload  = NULL,
};

/*============================================================================================*/

typedef struct platform_app_api_s
{
    void ( *app_function )( void );
} platform_app_api_t;

void
app_function( void )
{
    /* example function to export in the API struct */
}

static platform_app_api_t g_api = 
{
    .app_function = app_function
};

/*============================================================================================*/

module_api_t*
platform_app_get_module_api( void )
{
    return &g_module_api;
}

platform_app_api_t*
platform_app_get_api( void )
{
    return &g_api;
}

// void
// platform_app_module_register( void )
// {
//     // module_register_static( "platform_app", &g_platform_app_module_api, platform_app_get_api() );
//     // service_register( "platform_app", &g_platform_app_module_api, platform_app_get_api() );
// }

/*============================================================================================*/