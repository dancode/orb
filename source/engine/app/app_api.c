/*==============================================================================================

    engine/app/app_api.c — Platform-agnostic app module wiring.

==============================================================================================*/

/*==============================================================================================
    API struct
==============================================================================================*/

const app_api_t g_app_api_struct = {

    /* Window */
    .window_open              = app_window_open,
    .window_close             = app_window_close,
    .window_request_close     = app_window_request_close,
    .window_is_valid          = app_window_is_valid,
    .window_handle            = app_window_handle,
    .window_is_minimized      = app_window_is_minimized,
    .window_get_pos           = app_window_get_pos,
    .window_set_pos           = app_window_set_pos,
    .window_state             = app_window_state,
    .window_set_fillscreen    = app_window_set_fillscreen,
    .window_toggle_fillscreen = app_window_toggle_fillscreen,
    .window_resize            = app_window_resize,
    .window_minimize          = app_window_minimize,
    .window_restore           = app_window_restore,


    /* Native-borderless window actions */
    .window_start_move        = app_window_start_move,
    .window_start_resize      = app_window_start_resize,
    .window_title_event       = app_window_title_event,
    .window_system_menu       = app_window_system_menu,
    .window_enable_resize     = app_window_enable_resize,
    .window_maximize          = app_window_maximize,
    .window_toggle_maximize   = app_window_toggle_maximize,
    .window_set_native_frame  = app_window_set_native_frame,

    /* Event loop */
    .pump_events = app_pump_events,
    .next_event  = app_next_event,
    .should_quit = app_should_quit,

    /* Keyboard */
    .key_down            = app_key_down,
    .key_pressed         = app_key_pressed,
    .key_pressed_repeat  = app_key_pressed_repeat,
    .key_released        = app_key_released,

    /* Mouse */
    .mouse_position        = app_mouse_position,
    .mouse_position_screen = app_mouse_position_screen,
    .mouse_button_down     = app_mouse_button_down,
    .mouse_button_pressed  = app_mouse_button_pressed,
    .mouse_button_released = app_mouse_button_released,

    /* Clipboard */
    .clipboard_set = app_clipboard_set,
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

mod_desc_t*
app_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0, /* state lives in static storage in the platform backends */
        .func_api_size = sizeof( app_api_t ),
        .func_api      = &g_app_api_struct,
        .dep_count     = 0,
        .deps          = {}, 
        .init          = app_mod_init,
        .exit          = app_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
