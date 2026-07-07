// gui.vs.hlsl -- HLSL twin of gui.vert (the cooked-shader path).
//
// Cooked by shader_tool (via cook_shaders.bat) into bin/shaders/gui.vs.oshd; gui_render_init
// prefers the cooked pair when both files sit next to the exe and falls back to the embedded
// SPIR-V in pipeline/gui_shader.h (compiled from the GLSL twins) when they are absent.  Keep
// this file, the GLSL, and gui_push_t in gui_render.c in lockstep -- the push constant block
// and vertex inputs must stay byte-identical or the cooked and fallback paths diverge.
//
// The mvp is authored in VULKAN clip space (render_ortho maps top-left to -1,-1 with +y down),
// which is why the GLSL twin compiles without any y flip.  shader_tool bakes -fvk-invert-y
// into every vertex-stage cook (house convention: HLSL sources are D3D +y-up), so this shader
// negates y once to cancel it and match the fallback exactly.

struct gui_pc_t
{
    float4x4 mvp;        // column-major pixel-space ortho (Vulkan clip space)
    uint     tex_idx;    // bindless texture slot
    uint     samp_idx;   // bindless sampler slot
    uint     dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color
    uint     dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    uint     rgba_tex;   // 1 = sample tex_idx as a full RGBA image, not R8 coverage
};
[[vk::push_constant]] gui_pc_t pc;

struct vs_in_t
{
    [[vk::location( 0 )]] float2 pos   : POSITION;
    [[vk::location( 1 )]] float2 uv    : TEXCOORD0;
    [[vk::location( 2 )]] float4 color : COLOR0;    // fed by a UNORM4 attrib -> normalized float4
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
    o.sv_pos   = mul( pc.mvp, float4( v.pos, 0.0, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    o.color    = v.color;
    o.uv       = v.uv;
    return o;
}
