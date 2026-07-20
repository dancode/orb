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
