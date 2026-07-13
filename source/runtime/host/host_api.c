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

static run_clock_t       g_clock       = { .time_scale = 1.0f };
static run_frame_stats_t g_stats       = { 0 };
static u64               s_frame_count = 0;
static u64               s_prev_us     = 0;
static bool              s_started     = false;

#define RUN_MAX_DT_US 250000ull /* 0.25 s */

/*==============================================================================================
    Host-internal clock update (host.c calls this once per frame before on_update)
==============================================================================================*/

void
run_clock_update( u64 now_us )
{
    /* Integer tick in; floats are derived at this boundary only, never accumulated.
       The first frame has no previous stamp -- dt is 0, not time-since-boot. */
    u64 dt_us = s_started ? now_us - s_prev_us : 0;
    s_started = true;
    s_prev_us = now_us;

    u64 capped_us = dt_us > RUN_MAX_DT_US ? RUN_MAX_DT_US : dt_us;

    g_clock.app_time     = ( f64 )now_us * 1e-6;
    g_clock.app_time_us  = now_us;
    g_clock.dt_real      = ( f32 )( ( f64 )dt_us * 1e-6 );
    g_clock.dt           = ( f32 )( ( f64 )capped_us * 1e-6 ) * g_clock.time_scale;
    g_clock.frame_number = s_frame_count++;
}

void
run_clock_stats_submit( const run_frame_stats_t* stats )
{
    g_stats = *stats;
}

/*==============================================================================================
    API implementation
==============================================================================================*/

static const run_clock_t*
run_clock_impl( void )
{
    return &g_clock;
}

static const run_frame_stats_t*
run_frame_stats_impl( void )
{
    return &g_stats;
}

static void
run_set_time_scale_impl( f32 scale )
{
    g_clock.time_scale = scale;
}

/* Hot-reloaded gameplay DLLs cannot link the host-exe run_host_quit() symbol directly;
   this routes the same flag set through the run module's vtable. */
static void
run_request_quit_impl( void )
{
    run_host_quit();
}

/*==============================================================================================
    API struct
==============================================================================================*/

const run_api_t g_run_api_struct = {
    .clock          = run_clock_impl,
    .frame_stats    = run_frame_stats_impl,
    .set_time_scale = run_set_time_scale_impl,
    .request_quit   = run_request_quit_impl,
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
    g_stats       = ( run_frame_stats_t ){ 0 };
    s_frame_count = 0;
    s_prev_us     = 0;
    s_started     = false;
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
        .dep_count     = 5,
        .deps          = { "mod", "sys", "ref", "prof", "core" },
        .init          = run_mod_init,
        .exit          = run_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

// MOD_DEFINE_EXPORTS( run )

/*============================================================================================*/
