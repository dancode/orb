/*==============================================================================================
    sys state
==============================================================================================*/

typedef struct sys_state_s  
{
    int32_t  window_count;

} sys_state_t;

/*==============================================================================================
    sys lifecycle   
==============================================================================================*/

static bool
sys_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );  
    UNUSED( raw_state );  
    sys_state_t* s = raw_state;
    UNUSED( s );  
    return true;
}

static void
sys_tick( void* raw_state, float dt )
{
    UNUSED( dt );  
    sys_state_t* s = raw_state;
    UNUSED( s );
}

static void
sys_exit( void* raw_state )
{
    sys_state_t* s = raw_state;
    UNUSED( s );
}

/*==============================================================================================
    Platform API
==============================================================================================*/

const sys_api_t g_sys_api_struct = {
    .tick_reset        = sys_tick_reset,
    .tick_seconds      = sys_tick_seconds,
    .tick_microseconds = sys_tick_microseconds,
    .tick_milliseconds = sys_tick_milliseconds,
    .tick_nanoseconds  = sys_tick_nanoseconds,
    .tick_sleep        = sys_tick_sleep,
};

mod_api_t*
sys_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( sys_state_t ),

        .deps       = { "core" },
        .dep_count  = 1,

        .func_api   = &g_sys_api_struct,

        .init       = sys_init,
        .tick       = sys_tick,
        .exit       = sys_exit,
        .reload  = NULL,
    };
    return &api;
}

void*
sys_get_api( void )
{
    return (void*)&g_sys_api_struct;
}

/*============================================================================================*/