#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    uint tex_idx;
    uint samp_idx;
} pc;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main()
{
    gl_Position = pc.mvp * vec4( in_pos, 0.0, 1.0 );
    v_color = in_color;
    v_uv    = in_uv;
}
