/*==============================================================================================

    runtime_service/input/input_api.c -- Input API struct wiring + module descriptor.

    Included LAST by input.c.  input_actions.c has defined every static function in this
    translation unit; here they are assigned into the vtable and wrapped in the mod_desc_t
    lifecycle used by mod_static_load.

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const input_api_t g_input_api_struct =
{
    .action_register = input_action_register,
    .action_find     = input_action_find,
    .action_count    = input_action_count,
    .action_name     = input_action_name,

    .frame           = input_frame,

    .down            = input_down,
    .pressed         = input_pressed,
    .released        = input_released,
    .value           = input_value,
    .value2          = input_value2,

    .context_push    = input_context_push,
    .context_pop     = input_context_pop,
    .context_active  = input_context_active,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
input_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );

    /* Cache siblings; the deps "core"/"app" guarantee both are initialized first.
       core = cmd registry (the +/- transport); app = device axes (next phase). */
    if ( !MOD_FETCH_CORE )
        return false;
    if ( !MOD_FETCH_APP )
        return false;

    input_system_init();
    return true;
}

static bool
input_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    return MOD_FETCH_CORE && MOD_FETCH_APP;    // re-cache sibling API pointers after a hot-swap
}

static void
input_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    input_system_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
input_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,        /* registry state lives in file-scope globals */
        .func_api_size = sizeof( input_api_t ),
        .dep_count     = 2,
        .deps          = { "core", "app" },
        .func_api      = &g_input_api_struct,
        .init          = input_mod_init,
        .reload        = input_mod_reload,
        .exit          = input_mod_exit,
    };
    return &desc;
}

// clang-format on
/*============================================================================================*/
