#version 450

// Solid / solid-depth vertex shader.  Reads only pos (location 0) and color (location 1);
// the shared vertex also carries uv at location 2, which this pipeline ignores.

layout(push_constant) uniform PC {
    mat4 mvp;   // column-major view-projection
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 v_color;

void main()
{
    gl_Position = pc.mvp * vec4( in_pos, 1.0 );
    v_color     = in_color;
}
