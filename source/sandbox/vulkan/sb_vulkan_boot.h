/*==============================================================================================

    sandbox/vulkan/sb_vulkan_boot.h -- Bootstrap triangle render pass.

    Hardcoded single-triangle pipeline: positions in the vertex shader via gl_VertexIndex,
    solid orange-ish fragment color.  No vertex buffers, no depth attachment.

    DXC override (shader_tool Phase 0 proof): if sb_tri.vs.spv / sb_tri.ps.spv sit next to
    the executable, they are loaded instead of the embedded SPIR-V and the triangle renders
    BLUE via a push-constant tint.  Bake them with:

        bin\shader_tool.exe compile source\tools\shader_tool\test\sb_tri.vs.hlsl -o bin\sb_tri.vs.spv -T vs_6_0
        bin\shader_tool.exe compile source\tools\shader_tool\test\sb_tri.ps.hlsl -o bin\sb_tri.ps.spv -T ps_6_0

    Delete the .spv pair to fall back to the embedded orange triangle.

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
    bool           dxc;      /* true = shaders came from the dxc .spv override on disk */

} sb_vk_boot_t;

/* Create shaders and pipeline.  Returns false and logs on failure. */
bool sb_vk_boot_create( sb_vk_boot_t* boot );

/* Issue one full render pass that clears to dark green and draws the triangle. */
void sb_vk_boot_render( sb_vk_boot_t* boot, rhi_cmd_t cmd, i32 win_w, i32 win_h );

/* Destroy shaders and pipeline.  Safe to call even if create partially failed. */
void sb_vk_boot_destroy( sb_vk_boot_t* boot );

#endif /* SB_VULKAN_BOOT_H */
