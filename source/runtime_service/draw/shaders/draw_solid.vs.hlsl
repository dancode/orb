// draw_solid.vs.hlsl -- HLSL twin of draw_solid.vert (the cooked-shader path).
//
// Cooked by shader_tool (via cook_shaders.bat) into bin/shaders/draw_solid.vs.oshd;
// draw_material_init prefers the cooked pair when both files sit next to the exe and falls
// back to the embedded SPIR-V in draw_shader.h when they are absent.  Keep this file, the
// GLSL, and draw_push_t in lockstep.
//
// Reads only pos (location 0) and color (location 1); the shared vertex also carries uv at
// location 2, which this pipeline ignores.  The mvp is authored in Vulkan clip space (the
// GLSL twin compiles without a y flip), so negate y once to cancel the cook's baked
// -fvk-invert-y -- see gui_quad.vs.hlsl for the full story.

struct draw_pc_t
{
    float4x4 mvp;    // column-major view-projection (Vulkan clip space)
};
[[vk::push_constant]] draw_pc_t pc;

struct vs_in_t
{
    [[vk::location( 0 )]] float3 pos   : POSITION;
    [[vk::location( 1 )]] float4 color : COLOR0;
};

struct vs_out_t
{
    float4 sv_pos : SV_Position;
    float4 color  : COLOR0;
};

vs_out_t main( vs_in_t v )
{
    vs_out_t o;
    o.sv_pos   = mul( pc.mvp, float4( v.pos, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    o.color    = v.color;
    return o;
}
