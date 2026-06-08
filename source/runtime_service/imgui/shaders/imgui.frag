#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler   u_samplers[];

layout(push_constant) uniform PC {
    mat4 mvp;
    uint tex_idx;
    uint samp_idx;
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 s = texture( sampler2D( u_textures[pc.tex_idx], u_samplers[pc.samp_idx] ), v_uv );
    out_color = vec4( v_color.rgb, v_color.a * s.r );
}
