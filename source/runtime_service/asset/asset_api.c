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
    .refresh       = asset_refresh,
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

    /* Cache siblings; the deps "fs"/"core"/"rhi" guarantee all are initialized first.
       fs = read bytes by vpath; core = sid + alloc (registry); rhi = texture create/upload/
       bindless (image loader). */
    if ( !MOD_FETCH_FS )
        return false;
    if ( !MOD_FETCH_CORE )
        return false;
    if ( !MOD_FETCH_RHI )
        return false;

    asset_system_init();

    /* Built-in image type: acquire()ing any of these extensions decodes via stb_image and
       uploads a bindless texture (loaders/asset_image.c).  Registered here rather than by the
       caller so images "just work"; game/editor DLLs still add their own kinds via the API. */
    static const char* const image_exts[] = ASSET_IMAGE_EXTS;
    asset_type_register( "image", image_exts, ( u32 )( sizeof( image_exts ) / sizeof( image_exts[ 0 ] ) ),
                         asset_image_load, asset_image_unload, NULL );

    /* Built-in shader type: a cooked .oshd parses through the RHI's container loader
       (loaders/asset_shader.c); get() returns asset_shader_t with the handle + layout hash. */
    static const char* const shader_exts[] = ASSET_SHADER_EXTS;
    asset_type_register( "shader", shader_exts, ( u32 )( sizeof( shader_exts ) / sizeof( shader_exts[ 0 ] ) ),
                         asset_shader_load, asset_shader_unload, NULL );
    return true;
}

static bool
asset_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    return MOD_FETCH_FS && MOD_FETCH_CORE && MOD_FETCH_RHI;    // re-cache sibling API pointers after a hot-swap
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
        .dep_count     = 3,
        .deps          = { "fs", "core", "rhi" },
        .func_api      = &g_asset_api_struct,
        .init          = asset_mod_init,
        .reload        = asset_mod_reload,
        .exit          = asset_mod_exit,
    };
    return &desc;
}

// clang-format on
/*============================================================================================*/
