/*==============================================================================================

    shader_tool/test/sb_tri.vs.hlsl -- Phase 0 proof vertex shader.

    HLSL twin of the hand-assembled SPIR-V boot triangle in sb_vulkan_boot.c: three positions
    hard-coded in the shader, indexed by SV_VertexID, no vertex buffers.

    Positions are authored in the D3D convention (+Y up in clip space); shader_tool compiles
    vertex-ish stages with -fvk-invert-y, so this lands identically to the boot triangle's
    Y-down GLSL coordinates.

    Compile:
        shader_tool compile sb_tri.vs.hlsl -o sb_tri.vs.spv -T vs_6_0

==============================================================================================*/

static const float2 k_pos[ 3 ] =
{
    float2(  0.0,  0.5 ),   /* top center   */
    float2(  0.5, -0.5 ),   /* bottom right */
    float2( -0.5, -0.5 ),   /* bottom left  */
};

float4 main( uint vid : SV_VertexID ) : SV_Position
{
    return float4( k_pos[ vid ], 0.0, 1.0 );
}
