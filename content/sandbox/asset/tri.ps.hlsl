/*==============================================================================================

    shader_tool/test/sb_tri.ps.hlsl -- Phase 0 proof pixel shader.

    Outputs a solid color taken from a push constant, proving that a dxc-compiled
    [[vk::push_constant]] block is compatible with the RHI's shared pipeline layout
    (one range, all stages, RHI_MAX_PUSH_CONST_SIZE bytes) and that CPU-pushed data
    lands at the offsets the shader expects.

    CPU-side struct (sb_vulkan_boot.c): f32 tint[4] -- 16 bytes at offset 0.

    Compile:
        shader_tool compile sb_tri.ps.hlsl -o sb_tri.ps.spv -T ps_6_0

==============================================================================================*/

struct tri_pc_t
{
    float4 tint;
};

[[vk::push_constant]] tri_pc_t pc;

float4 main() : SV_Target0
{
    return pc.tint;
}
