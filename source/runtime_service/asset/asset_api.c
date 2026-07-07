/*==============================================================================================

    runtime_service/asset/asset_api.c -- Asset API struct wiring + module descriptor.

    Included LAST by asset.c.  asset_registry.c has defined every static function in this
    translation unit; here they are assigned into the vtable and wrapped in the mod_desc_t
    lifecycle used by mod_static_load.

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const asset_api_t g_asset_api_struct =
{
    .type_register = asset_type_register,
    .acquire       = asset_acquire,
    .release       = asset_release,
    .reload        = asset_reload,
    .get           = asset_get,
    .state         = asset_state,
    .valid         = asset_valid,
    .refcount      = asset_refcount,
    .count         = asset_count,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
asset_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );

    /* Cache core (fs + sid + alloc); the dep "core" guarantees it is initialized first. */
    if ( !MOD_FETCH_CORE )
        return false;

    asset_system_init();
    return true;
}

static bool
asset_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    return MOD_FETCH_CORE;    // re-cache the sibling API pointer after a hot-swap
}

static void
asset_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    asset_system_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
asset_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,        /* registry state lives in file-scope globals */
        .func_api_size = sizeof( asset_api_t ),
        .dep_count     = 1,
        .deps          = { "core" },
        .func_api      = &g_asset_api_struct,
        .init          = asset_mod_init,
        .reload        = asset_mod_reload,
        .exit          = asset_mod_exit,
    };
    return &desc;
}

// clang-format on
/*============================================================================================*/
