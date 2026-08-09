#version 450

// sb_vulkan_stress -- textured quad vertex shader.
//
// Owned by the stress sandbox, deliberately NOT shared with gui or draw.  This pipeline exists
// to give the RHI test a real textured draw (bindless sample -> QFOT acquire -> shader read);
// borrowing another subsystem's shader couples an RHI test to that subsystem's vertex format,
// which is exactly how the previous gui_shader.h dependency broke when gui repacked its vertex.
//
// Vertex layout is the sandbox's own f4_vert_t (20 bytes) -- keep the two in step:
//     location 0  pos    float2      offset  0
//     location 1  uv     float2      offset  8
//     location 2  color  unorm8x4    offset 16

layout(push_constant) uniform PC {
    mat4 mvp;      // unit quad [0,1]^2 -> pixel rect -> Vulkan NDC
    uint tex_idx;  // bindless texture slot
    uint samp_idx; // bindless sampler slot
} pc;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    gl_Position = pc.mvp * vec4( in_pos, 0.0, 1.0 );
    v_uv        = in_uv;
    v_color     = in_color;
}
