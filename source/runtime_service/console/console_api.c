/*==============================================================================================

    runtime_service/console/console_api.c -- Console API struct wiring + module descriptor.

    Included LAST by console.c.  console_view.c has defined every static function in this
    translation unit; here they are assigned into the vtable and wrapped in the mod_desc_t
    lifecycle used by mod_static_load.

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const console_api_t g_console_api_struct =
{
    .frame    = console_frame,
    .emit     = console_emit,

    .set_open = console_set_open,
    .toggle   = console_toggle,
    .is_open  = console_is_open,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
console_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );

    /* Cache siblings; the deps "core"/"app"/"gui" guarantee all three are initialized first.
       core = the con_* backend; app = the toggle keys; gui = the drop-down front end. */
    if ( !MOD_FETCH_CORE )
        return false;
    if ( !MOD_FETCH_APP )
        return false;
    if ( !MOD_FETCH_GUI )
        return false;

    /* Service-owned cvars (view state stays in file-scope globals; these two drive presentation).
       Both CVAR_ARCHIVE so writeconfig persists them.  con_height is a viewport fraction snapped
       to whole rows in console_show; con_log_level is a string enum applied to core's ambient-log
       floor in console_frame. */
    s_cv_height = core()->cvar_register_f(
        "con_height", "Developer console height as a fraction of the viewport (0.1..1.0, 1.0 = full).",
        0.4f, 0.1f, 1.0f, CVAR_ARCHIVE );

    static const char* k_log_levels[] = { "trace", "debug", "info", "warn", "error" };
    s_cv_log_level = core()->cvar_register_s(
        "con_log_level", "Minimum severity of ambient log lines shown in the console.",
        k_log_levels, 5, 3 /* default: warn */, CVAR_ARCHIVE );

    core()->cmd_register( "toggleconsole", console_cmd_toggle, "Toggle the developer console." );
    core()->cmd_register( "condump",       console_cmd_dump,   "Copy the console scrollback to the clipboard." );

    return true;
}

static bool
console_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    return MOD_FETCH_CORE && MOD_FETCH_APP && MOD_FETCH_GUI;    // re-cache sibling APIs after a hot-swap
}

static void
console_mod_exit( void* raw_state )
{
    UNUSED( raw_state );

    /* Drop the commands registered in init (the cvar system has no unregister and tears down with
       core).  Guarded: core() is always valid for a static service, but stay defensive. */
    if ( core() )
    {
        core()->cmd_unregister( "toggleconsole" );
        core()->cmd_unregister( "condump" );
    }
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
console_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,        /* view state lives in file-scope globals */
        .func_api_size = sizeof( console_api_t ),
        .dep_count     = 3,
        .deps          = { "core", "app", "gui" },
        .func_api      = &g_console_api_struct,
        .init          = console_mod_init,
        .reload        = console_mod_reload,
        .exit          = console_mod_exit,
    };
    return &desc;
}

// clang-format on
/*============================================================================================*/
