/*==============================================================================================

    sandbox/vulkan/sb_vulkan_boot.h -- Bootstrap triangle render pass.

    Hardcoded single-triangle pipeline: positions in the vertex shader via gl_VertexIndex,
    solid orange-ish fragment color.  No vertex buffers, no depth attachment.

    Usage:
        sb_vk_boot_t boot = {0};
        sb_vk_boot_create( &boot );           // call once after rhi()->init()

        // inside render loop, after frame_begin():
        sb_vk_boot_render( &boot, cmd, win_w, win_h );

        sb_vk_boot_destroy( &boot );          // call before rhi()->shutdown()

==============================================================================================*/
#ifndef SB_VULKAN_BOOT_H
#define SB_VULKAN_BOOT_H

#include "orb.h"
#include "runtime_service/rhi/rhi.h"

typedef struct
{
    rhi_shader_t   vert;
    rhi_shader_t   frag;
    rhi_pipeline_t pipeline;
} sb_vk_boot_t;

/* Create shaders and pipeline.  Returns false and logs on failure. */
bool sb_vk_boot_create( sb_vk_boot_t* boot );

/* Issue one full render pass that clears to dark green and draws the triangle. */
void sb_vk_boot_render( sb_vk_boot_t* boot, rhi_cmd_list_t cmd, i32 win_w, i32 win_h );

/* Destroy shaders and pipeline.  Safe to call even if create partially failed. */
void sb_vk_boot_destroy( sb_vk_boot_t* boot );

#endif /* SB_VULKAN_BOOT_H */
