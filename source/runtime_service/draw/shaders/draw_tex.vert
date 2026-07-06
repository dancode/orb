#version 450

// Textured vertex shader.  Reads all three shared attributes and forwards the tint + uv.

layout(push_constant) uniform PC {
    mat4 mvp;      // column-major view-projection
    uint tex_idx;  // bindless texture slot
    uint samp_idx; // bindless sampler slot
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main()
{
    gl_Position = pc.mvp * vec4( in_pos, 1.0 );
    v_color     = in_color;
    v_uv        = in_uv;
}
