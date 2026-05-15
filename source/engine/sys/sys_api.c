/*==============================================================================================

    sys_api.c — Platform-agnostic sys module wiring.

    Implements the sys_api_t function-pointer struct and the mod_api_t lifecycle descriptor.

==============================================================================================*/
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
    Persistent state (allocated by module init)
==============================================================================================*/

typedef struct sys_state_s
{
    int32_t no_state;

} sys_state_t;

static sys_state_t* s = NULL;

/*==============================================================================================
    API Struct
==============================================================================================*/

const sys_api_t g_sys_api_struct = {
    .tick_seconds       = sys_tick_seconds,
    .tick_microseconds  = sys_tick_microseconds,
    .tick_milliseconds  = sys_tick_milliseconds,
    .tick_nanoseconds   = sys_tick_nanoseconds,
    .sleep_milliseconds = sys_sleep_milliseconds,
};

/*==============================================================================================
    Module Lifecycle (called by the module system)
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
        .func_api      = &g_sys_api_struct,
        .deps          = NULL,
        .dep_count     = 0,        
        .init          = sys_mod_init,
        .exit          = sys_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/