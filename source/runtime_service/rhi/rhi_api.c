/*==============================================================================================

    runtime/services/rhi/rhi_api.c -- RHI API struct wiring + module descriptor.

    Included LAST by rhi.c.  By this point every vk_*.c file has defined all static
    functions in the same translation unit; this file only assigns them into the vtable
    and provides the mod_desc_t lifecycle descriptor for mod_static_load.

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const rhi_api_t g_rhi_api_struct =
{
    /* Global lifecycle */
    .init                       = vk_init,
    .shutdown                   = vk_shutdown,

    /* Per-context lifecycle */
    .context_create             = vk_context_create,
    .context_destroy            = vk_context_destroy,
    .context_resize             = vk_context_resize,

    /* Frame */
    .frame_begin                = vk_frame_begin,
    .frame_end                  = vk_frame_end,

    /* Buffer */
    .buffer_create              = vk_buffer_create,
    .buffer_destroy             = vk_buffer_destroy,
    .buffer_write               = vk_buffer_write,
    .buffer_get_device_address  = vk_buffer_get_device_address,

    /* Texture */
    .texture_create             = vk_texture_create,
    .texture_destroy            = vk_texture_destroy,

    /* Sampler */
    .sampler_create             = vk_sampler_create,
    .sampler_destroy            = vk_sampler_destroy,

    /* Shader */
    .shader_create              = vk_shader_create,
    .shader_destroy             = vk_shader_destroy,
    .shader_load_file           = vk_shader_load_file,
    .shader_load_memory         = vk_shader_load_memory,

    /* Pipeline */
    .pipeline_create            = vk_pipeline_create,
    .compute_pipeline_create    = vk_compute_pipeline_create,
    .pipeline_destroy           = vk_pipeline_destroy,

    /* Staged upload */
    .upload_buffer              = vk_upload_buffer,
    .upload_texture             = vk_upload_texture,

    /* Render pass */
    .cmd_begin_rendering        = vk_cmd_begin_rendering,
    .cmd_end_rendering          = vk_cmd_end_rendering,

    /* Commands */
    .cmd_set_viewport           = vk_cmd_set_viewport,
    .cmd_set_scissor            = vk_cmd_set_scissor,
    .cmd_bind_pipeline          = vk_cmd_bind_pipeline,
    .cmd_bind_vertex_buffer     = vk_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer      = vk_cmd_bind_index_buffer,
    .cmd_push_constants         = vk_cmd_push_constants,
    .cmd_draw                       = vk_cmd_draw,
    .cmd_draw_indexed               = vk_cmd_draw_indexed,
    .cmd_draw_indirect              = vk_cmd_draw_indirect,
    .cmd_draw_indexed_indirect      = vk_cmd_draw_indexed_indirect,
    .cmd_dispatch                   = vk_cmd_dispatch,

    /* Bindless */
    .register_texture           = vk_register_texture,
    .unregister_texture         = vk_unregister_texture,
    .register_sampler           = vk_register_sampler,
    .unregister_sampler         = vk_unregister_sampler,
    .cmd_bind_bindless          = vk_cmd_bind_bindless,

    /* Debug labels */
    .cmd_begin_label            = vk_cmd_begin_label,
    .cmd_end_label              = vk_cmd_end_label,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
rhi_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );

    /* Load Vulkan DLL only.  Host calls rhi()->init() for the global device, then
       rhi()->context_create() per window. */
    return vk_lib_init();
}

static void
rhi_mod_exit( void* raw_state )
{
    UNUSED( raw_state );

    /* Defensive cleanup: destroy any contexts the host left open, then shut down. */
    for ( int i = 0; i < RHI_CTX_MAX; ++i )
    {
        if ( vk.ctx_alloc & ( 1u << i ) )
            vk_context_destroy( i );
    }

    if ( vk.initialized )
        vk_shutdown();

    vk_lib_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
rhi_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,   /* singleton lives in vk (vk_state.c); not managed by mod system */
        .func_api_size = sizeof( rhi_api_t ),
        .dep_count     = 3,
        .deps          = { "sys", "app", "core" },
        .func_api      = &g_rhi_api_struct,
        .init          = rhi_mod_init,
        .exit          = rhi_mod_exit,
        .reload        = NULL,   /* RHI is a static service; hot-reload not supported */
    };
    return &desc;
}

/*============================================================================================*/
// clang-format on
