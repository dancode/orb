/*==============================================================================================

    host_api.c — runtime module implementation.

    Maintains the authoritative frame clock. host.c calls run_clock_update() once
    per frame before dispatching on_update. The capping and time-scale logic lives
    here so the host loop stays minimal. Modules that need more than a plain f32 dt
    can call run()->clock() to read app_time, frame_number, time_scale, etc.

==============================================================================================*/
/*==============================================================================================
    Internal state
==============================================================================================*/

static run_clock_t g_clock      = { .time_scale = 1.0f };
static u64         s_frame_count = 0;

#define RUN_MAX_DT 0.25f

/*==============================================================================================
    Host-internal clock update (host.c calls this once per frame before on_update)
==============================================================================================*/

void
run_clock_update( f64 app_time, f32 dt_real )
{
    f32 capped       = dt_real > RUN_MAX_DT ? RUN_MAX_DT : dt_real;
    g_clock.app_time    = app_time;
    g_clock.dt_real     = dt_real;
    g_clock.dt          = capped * g_clock.time_scale;
    g_clock.frame_number = s_frame_count++;
}

/*==============================================================================================
    API implementation
==============================================================================================*/

static const run_clock_t*
run_clock_impl( void )
{
    return &g_clock;
}

static void
run_set_time_scale_impl( f32 scale )
{
    g_clock.time_scale = scale;
}

/*==============================================================================================
    API struct
==============================================================================================*/

const run_api_t g_run_api_struct = {
    .clock          = run_clock_impl,
    .set_time_scale = run_set_time_scale_impl,
};

/*==============================================================================================
    Module lifecycle
==============================================================================================*/

static bool
run_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    g_clock       = ( run_clock_t ){ .time_scale = 1.0f };
    s_frame_count = 0;
    return true;
}

static void
run_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
run_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( run_api_t ),
        .func_api      = &g_run_api_struct,
        .deps          = NULL,
        .dep_count     = 0,
        .init          = run_mod_init,
        .exit          = run_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

MOD_DEFINE_EXPORTS( run )

/*============================================================================================*/
