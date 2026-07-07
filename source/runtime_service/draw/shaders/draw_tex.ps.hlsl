// draw_tex.ps.hlsl -- HLSL twin of draw_tex.frag (the cooked-shader path).
//
// Samples a full-RGBA bindless texture and modulates it by the vertex tint.  The texel
// arrives LINEAR (an _SRGB-format view is hardware-decoded at sample time), so only the
// authored tint (sRGB) is decoded before the multiply.

struct draw_tex_pc_t
{
    float4x4 mvp;         // column-major view-projection (Vulkan clip space)
    uint     tex_idx;     // bindless texture slot
    uint     samp_idx;    // bindless sampler slot
};
[[vk::push_constant]] draw_tex_pc_t pc;

[[vk::binding( 0, 0 )]] Texture2D    u_textures[] : register( t0, space0 );
[[vk::binding( 1, 0 )]] SamplerState u_samplers[] : register( s0, space0 );

struct ps_in_t
{
    float4 sv_pos : SV_Position;
    float4 color  : COLOR0;
    float2 uv     : TEXCOORD0;
};

float3 srgb_to_linear( float3 c )
{
    float3 lo = c / 12.92;
    float3 hi = pow( ( c + 0.055 ) / 1.055, 2.4 );
    return lerp( hi, lo, step( c, 0.04045 ) );    // c <= 0.04045 selects lo (GLSL mix + cutoff)
}

float4 main( ps_in_t i ) : SV_Target0
{
    float4 s = u_textures[ NonUniformResourceIndex( pc.tex_idx ) ].Sample(
                   u_samplers[ NonUniformResourceIndex( pc.samp_idx ) ], i.uv );
    return float4( s.rgb * srgb_to_linear( i.color.rgb ), s.a * i.color.a );
}
