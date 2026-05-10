/*==============================================================================================

    sys_api.c : The platform-agnostic "system" module.

==============================================================================================*/

#include "engine/mod/mod_export.h"

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct sys_state_s
{
    int32_t window_count;

} sys_state_t;

/*==============================================================================================
    API Start / Shutdown
==============================================================================================*/

void
sys_init( void )
{
    sys_tick_init();
}

void
sys_exit( void )
{
    sys_tick_exit();
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const sys_api_t g_sys_api_struct = {
    .tick_reset         = sys_tick_reset,
    .tick_seconds       = sys_tick_seconds,
    .tick_microseconds  = sys_tick_microseconds,
    .tick_milliseconds  = sys_tick_milliseconds,
    .tick_nanoseconds   = sys_tick_nanoseconds,
    .sleep_milliseconds = sys_sleep_milliseconds,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
sys_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    UNUSED( raw_state );
    sys_init();
    return true;
}

static void
sys_mod_tick( void* raw_state, float dt )
{
    UNUSED( raw_state );
    UNUSED( dt );
}

static void
sys_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    sys_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
sys_get_mod_api( void )
{
    static mod_api_t api = {
        .version       = 1,
        .state_size    = sizeof( sys_state_t ),
        .func_api_size = sizeof( sys_api_t ),
        .deps          = NULL,
        .dep_count     = 0,
        .func_api      = &g_sys_api_struct,
        .init          = sys_mod_init,
        .tick          = sys_mod_tick,
        .exit          = sys_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/