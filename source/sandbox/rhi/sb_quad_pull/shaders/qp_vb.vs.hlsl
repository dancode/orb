// qp_vb.vs.hlsl -- the CLASSIC vertex stage of the sb_quad_pull proof: the control arm of the
// A/B.  The CPU expands every quad into six 20-byte vertices (qp_vert_t, sb_quad_pull.c)
// and this stage just transforms what arrives, exactly like today's gui vertex stage.  Cooked
// to bin/shaders/qp_vb.vs.oshd; qp_pull.vs.hlsl is the pulled stage measured against this one.
//
// The push constant block mirrors qp_push_t in sb_quad_pull.c; the buffer slots are unused
// here but the block must stay identical across the three shaders.

struct qp_pc_t
{
    float2 scale;        // pixel -> NDC scale (2/w, 2/h); Vulkan clip space, +y down
    float2 offset;       // pixel -> NDC offset (-1, -1)
    uint   quad_buf;     // bindless buffer slot of the quad records
    uint   quad_base;    // first record of this draw
    uint   style_buf;    // bindless buffer slot of the style records
    uint   style_base;   // first style of this draw
};
[[vk::push_constant]] qp_pc_t pc;

// Two attributes are PACKED in memory (uv unorm16x2, color unorm8x4); vertex fetch widens them
// to float before the shader sees them, same as the gui pipeline.
struct vs_in_t
{
    [[vk::location( 0 )]] float2 pos   : POSITION;
    [[vk::location( 1 )]] float2 uv    : TEXCOORD0;
    [[vk::location( 2 )]] float4 color : COLOR0;
    [[vk::location( 3 )]] uint   style : TEXCOORD1;
};

struct vs_out_t
{
    float4                sv_pos : SV_Position;
    float4                color  : COLOR0;
    float2                uv     : TEXCOORD0;
    nointerpolation uint  style  : TEXCOORD1;
};

vs_out_t main( vs_in_t v )
{
    vs_out_t o;
    o.sv_pos   = float4( v.pos * pc.scale + pc.offset, 0.0, 1.0 );
    o.sv_pos.y = -o.sv_pos.y;   // cancel the cook's -fvk-invert-y: pc maps +y down already
    o.color    = v.color;
    o.uv       = v.uv;
    o.style    = v.style;
    return o;
}
