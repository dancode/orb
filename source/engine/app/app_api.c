/*==============================================================================================

    engine/app/app_api.c — Platform-agnostic app module wiring.

==============================================================================================*/
/*==============================================================================================
    Init / Exit
==============================================================================================*/

/* Module-level lifecycle is intentionally empty for v0. The host owns the
   window lifetime via window_create / window_destroy inside on_init / on_exit,
   so app's mod_init has nothing to do. Memory and similar future subsystems
   would be initialized here. */

/*==============================================================================================
    Persistent state (allocated by module init)
==============================================================================================*/

typedef struct app_state_s
{
    int32_t window_count;

} app_state_t;

static app_state_t* s = NULL;

/*==============================================================================================
    API Struct
==============================================================================================*/

const app_api_t g_app_api_struct = {

    /* Window */
    .window_create  = app_window_create,
    .window_destroy = app_window_destroy,
    .pump_events    = app_pump_events,
    .window_handle  = app_window_handle,

    /* Keyboard */
    .key_down     = app_key_down,
    .key_pressed  = app_key_pressed,
    .key_released = app_key_released,

    /* Mouse */
    .mouse_position        = app_mouse_position,
    .mouse_button_down     = app_mouse_button_down,
    .mouse_button_pressed  = app_mouse_button_pressed,
    .mouse_button_released = app_mouse_button_released,
};

/*==============================================================================================
    Module lifecycle  (called by the module system)
==============================================================================================*/

static bool
app_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    return true;
}

static void
app_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
app_get_mod_api( void )
{
    static mod_api_t api = {
        .version       = 1,
        .state_size    = 0, /* stateless at module level — window state lives in win_window.c */
        .func_api_size = sizeof( app_api_t ),
        .func_api      = &g_app_api_struct,
        .dep_count     = 1,
        .deps          = { "sys" }, /* engine-tier rule: depends only on sys */
        .init          = app_mod_init,
        .exit          = app_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/