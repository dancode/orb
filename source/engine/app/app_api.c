/*==============================================================================================

    engine/app/app_api.c — Platform-agnostic app module wiring.

==============================================================================================*/

/*==============================================================================================
    API struct
==============================================================================================*/

const app_api_t g_app_api_struct = {

    /* Window */
    .window_open         = app_window_open,
    .window_close        = app_window_close,
    .window_is_valid     = app_window_is_valid,
    .window_handle       = app_window_handle,
    .window_is_minimized      = app_window_is_minimized,
    .window_state             = app_window_state,
    .window_set_fillscreen    = app_window_set_fillscreen,
    .window_toggle_fillscreen = app_window_toggle_fillscreen,
    .window_set_paint         = app_window_set_paint,
    .window_toggle_paint      = app_window_toggle_paint,
    .window_paint_enabled     = app_window_paint_enabled,

    /* Event loop */
    .pump_events = app_pump_events,
    .next_event  = app_next_event,
    .should_quit = app_should_quit,

    /* Keyboard */
    .key_down     = app_key_down,
    .key_pressed  = app_key_pressed,
    .key_released = app_key_released,

    /* Mouse */
    .mouse_position        = app_mouse_position,
    .mouse_button_down     = app_mouse_button_down,
    .mouse_button_pressed  = app_mouse_button_pressed,
    .mouse_button_released = app_mouse_button_released,

    /* Input mode */
    .key_repeat_set = app_key_repeat_set,
};

/*==============================================================================================
    Module lifecycle
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
        .state_size    = 0, /* state lives in static storage in the platform backends */
        .func_api_size = sizeof( app_api_t ),
        .func_api      = &g_app_api_struct,
        .dep_count     = 1,
        .deps          = { "sys" },
        .init          = app_mod_init,
        .exit          = app_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
