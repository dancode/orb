/*==============================================================================================

    engine/mod/mod_api.c - Module system API struct and descriptor.

    Unity include for mod.c -- not compiled directly. All symbols defined here are visible
    within the mod.c translation unit.

==============================================================================================*/

/*==============================================================================================
    API struct
==============================================================================================*/

const mod_api_t g_mod_api_struct = {
    .dynamic_load = mod_dynamic_load,
    .unload       = mod_unload,
    .get_api      = mod_get_api,
    .reload       = mod_reload,
    .is_loaded    = mod_is_loaded,
    .each         = mod_each,
    .last_error   = mod_last_error,
};

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
mod_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( mod_api_t ),
        .func_api      = &g_mod_api_struct,
        .dep_count     = 0,
        .init          = NULL,
        .exit          = NULL,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
