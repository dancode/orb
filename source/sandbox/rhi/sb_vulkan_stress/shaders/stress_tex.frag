#version 450
#extension GL_EXT_nonuniform_qualifier : require

// sb_vulkan_stress -- textured quad fragment shader.  See stress_tex.vert for why the sandbox
// owns this pair instead of borrowing gui's or draw's.
//
// The whole point of the draw is the SAMPLE: it forces the queue-family-ownership-transfer
// acquire and the shader read of a freshly uploaded, freshly registered bindless texture.  So
// this stays as thin as possible -- anything beyond the fetch is noise in an RHI test.
//
// The sampled texture is RHI_FORMAT_RGBA8_UNORM holding a deterministic ramp (tex_data[i] =
// i*7+13), so the texel is already linear and needs no decode.  The vertex tint is the quad's
// solid white and is treated as linear for the same reason -- this is a test pattern, not
// authored art, so the sRGB round trip draw_tex.frag performs would only obscure the result.

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler   u_samplers[];

layout(push_constant) uniform PC {
    mat4 mvp;
    uint tex_idx;
    uint samp_idx;
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 1) in  vec4 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 s = texture( sampler2D( u_textures[nonuniformEXT( pc.tex_idx )],
                                 u_samplers[nonuniformEXT( pc.samp_idx )] ), v_uv );
    out_color = s * v_color;
}
