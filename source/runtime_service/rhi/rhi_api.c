/*==============================================================================================

    runtime/services/rhi/rhi_api.c -- RHI API struct wiring + module descriptor.

    Included LAST by rhi.c. By this point vk_*.c and vk_init.c have defined all
    static functions in the same translation unit. This file only:
        - Assigns vk_* / rhi_* functions into g_rhi_api_struct
        - Provides the mod_desc_t descriptor for mod_static_load

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const rhi_api_t g_rhi_api_struct = 
{
    /* Lifecycle */
    .init            = vk_init,
    .shutdown        = vk_shutdown,
    .resize          = vk_resize,

    /* Frame */
    .frame_begin     = vk_frame_begin,
    .frame_end       = vk_frame_end,

    /* Commands */
    .cmd_clear_color = vk_cmd_clear_color,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
rhi_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );

    /* Real device init happens in rhi()->init() once the host calls it with
       a window handle. */

    bool success = vk_lib_init();
    if ( success == false )
        return false;

    return true;
}

static void
rhi_mod_exit( void* raw_state )
{
    UNUSED( raw_state );

    /* Defensive: if the host forgot to call shutdown(), do it now. */
    if ( g_vk.initialized )
    {
        vk_shutdown();    
    }
    vk_lib_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
rhi_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0, /* singleton lives in vk_state.c's g_vk */
        .func_api_size = sizeof( rhi_api_t ),
        .dep_count     = 2,
        .deps          = { "sys", "app", "core" },
        .func_api      = &g_rhi_api_struct,
        .init          = rhi_mod_init,
        .exit          = rhi_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
// clang-format off