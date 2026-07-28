#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    uint tex_idx;
    uint samp_idx;
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    uint rgba_tex;   // 1 = sample tex_idx as a full RGBA image (scene viewport), not R8 coverage
} pc;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_fx_coord;   // effect coord: |p| - c, shape-local pixels
layout(location = 4) in uint in_fx;         // packed effect word; low nibble 0 = no effect

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_fx_coord;
// flat: the effect word names the SHAPE, which is constant over it -- interpolating a bit field
// would blend a radius into a mode.  Nothing else in the band needs to travel per fragment.
layout(location = 3) flat out uint v_fx;

void main()
{
    gl_Position = pc.mvp * vec4( in_pos, 0.0, 1.0 );
    v_color    = in_color;
    v_uv       = in_uv;
    v_fx_coord = in_fx_coord;
    v_fx       = in_fx;
}
