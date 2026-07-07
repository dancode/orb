// draw_tex.vs.hlsl -- HLSL twin of draw_tex.vert (the cooked-shader path).
//
// Reads all three shared attributes and forwards the tint + uv.  See draw_solid.vs.hlsl for
// the cooked-vs-fallback contract and the y-negate rationale.

struct draw_tex_pc_t
{
    float4x4 mvp;         // column-major view-projection (Vulkan clip space)
    uint     tex_idx;     // bindless texture slot
    uint     samp_idx;    // bindless sampler slot
};
[[vk::push_constant]] draw_tex_pc_t pc;

struct vs_in_t
{
    [[vk::location( 0 )]] float3 pos   : POSITION;
    [[vk::location( 1 )]] float4 color : COLOR0;
    [[vk::location( 2 )]] float2 uv    : TEXCOORD0;
};

struct vs_out_t
{
    float4 sv_pos : SV_Position;
    float4 color  : COLOR0;
    float2 uv     : TEXCOORD0;
};

vs_out_t main( vs_in_t v )
{
    vs_out_t o;
    o.sv_pos   = mul( pc.mvp, float4( v.pos, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    o.color    = v.color;
    o.uv       = v.uv;
    return o;
}
