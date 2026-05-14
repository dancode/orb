/*==============================================================================================

    runtime/app/runtime_app.c — Windowed application runtime implementation.

==============================================================================================*/

#include "orb.h"
#include "engine/core/core.h"
#include "engine/app/app.h"
#include "runtime/host/runtime_host.h"
#include "runtime/app/runtime_app.h"

/*==============================================================================================
    Internal state — routes user config through runtime_host's single userdata slot
==============================================================================================*/

typedef struct app_runner_state_s
{
    const rt_app_config_t* config;

} app_runner_state_t;

/*==============================================================================================
    Wrapper callbacks
==============================================================================================*/

static bool
app_runner_on_init( void* userdata )
{
    app_runner_state_t*    runner = ( app_runner_state_t* )userdata;
    const rt_app_config_t* cfg    = runner->config;

    const char*            title  = cfg->window.title ? cfg->window.title : RUNTIME_APP_DEFAULT_TITLE;
    i32                    w      = cfg->window.width > 0 ? cfg->window.width : RUNTIME_APP_DEFAULT_WIDTH;
    i32                    h      = cfg->window.height > 0 ? cfg->window.height : RUNTIME_APP_DEFAULT_HEIGHT;

    if ( app_api()->window_create( title, w, h ) == false )
    {
        core_api()->log_error( "[runtime_app] window_create failed" );
        return false;
    }

    if ( cfg->on_init && !cfg->on_init( cfg->userdata ) )
    {
        core_api()->log_error( "[runtime_app] user on_init returned false" );
        return false;
    }

    return true;
}

static bool
app_runner_on_update( float dt, void* userdata )
{
    app_runner_state_t*    runner = ( app_runner_state_t* )userdata;
    const rt_app_config_t* cfg    = runner->config;

    if ( app_api()->pump_events() == false )
        return false;

    return cfg->on_update( dt, cfg->userdata );
}

static void
app_runner_on_exit( void* userdata )
{
    app_runner_state_t*    runner = ( app_runner_state_t* )userdata;
    const rt_app_config_t* cfg    = runner->config;

    if ( cfg->on_exit )
        cfg->on_exit( cfg->userdata );

    app_api()->window_destroy();
}

/*==============================================================================================
    Public: runtime_app_run: main entry point for the app runtime.
==============================================================================================*/

bool
runtime_app_run( const rt_app_config_t* config )
{
    /* This function uses config to route the user callbacks through the single userdata slot of
       runtime_host_run. We assert here to catch config errors early, before the host starts up. */

    ORB_ASSERT_MSG( config != NULL, "runtime_app_run: config is NULL" );
    ORB_ASSERT_MSG( config->host_name != NULL, "runtime_app_run: host_name is NULL" );
    ORB_ASSERT_MSG( config->on_update != NULL, "runtime_app_run: on_update is required" );

    /* app_runner_state_t — a tiny stack - allocated struct that holds a pointer to the
       user's config. runtime_host sees this struct as its userdata; the wrapper callbacks 
       unpack it to  dispatch to the user's callbacks with the user's userdata. 
       This pattern is worth remembering because every future wrapper layer 
       (game framework above runtime_app, for instance) will use the same routing.
    */

    app_runner_state_t runner = { .config = config };

    /* We could build the runtime_host config here with the user callbacks as-is, but this wrapper
       approach lets us do setup and teardown around the user callbacks, e.g. creating the
       window before user init and destroying it after user exit. */

    const rt_config_t inner = {
        .host_name = config->host_name,
        .modules   = config->modules,

        .on_init   = app_runner_on_init,
        .on_update = app_runner_on_update,
        .on_exit   = app_runner_on_exit,

        /* the app callbacks get our config */

        .userdata          = &runner,

        .frame_target_ms   = config->frame_target_ms,
        .load_core         = true,             /* implicit */
        .load_app          = true,             /* implicit */
        .load_rhi          = config->load_rhi, /* passthrough */
        .enable_hot_reload = config->enable_hot_reload,
    };

    return runtime_host_run( &inner );
}

/*============================================================================================*/