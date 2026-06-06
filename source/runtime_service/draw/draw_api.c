/*==============================================================================================

    runtime_service/draw/draw_api.c -- Draw API struct wiring + module descriptor.

    Included LAST by draw.c.  By this point draw_geo.c, draw_batch.c, draw_material.c,
    and draw_cmd.c have all defined their static functions in the same translation unit;
    this file only assigns them into the vtable and provides the mod_desc_t lifecycle
    descriptor for mod_static_load / mod_dynamic_load.

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const draw_api_t g_draw_api_struct =
{
    .init     = draw_init,
    .shutdown = draw_shutdown,
    .begin    = draw_begin,
    .end      = draw_end,
    .rect     = draw_rect,
    .box      = draw_box,
    .circle   = draw_circle,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
draw_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );

    /* Cache the rhi API pointer; GPU resource creation is deferred to draw()->init()
       because the Vulkan device does not exist until the host calls rhi()->init(). */
    return MOD_FETCH_RHI;
}

static bool
draw_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );

    /* Re-cache sibling API pointers after a hot-swap of this DLL. */
    if ( !MOD_FETCH_RHI ) return false;

    return true;
}

static void
draw_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    draw_shutdown();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
draw_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,        /* module state lives in static globals in draw_cmd.c */
        .func_api_size = sizeof( draw_api_t ),
        .dep_count     = 1,
        .deps          = { "rhi" },
        .func_api      = &g_draw_api_struct,
        .init          = draw_mod_init,
        .reload        = draw_mod_reload,
        .exit          = draw_mod_exit,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( draw )

// clang-format on
/*============================================================================================*/
