#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Textured fragment shader.  Samples a full-RGBA bindless texture and modulates it by the
// vertex tint.  The texel arrives LINEAR (an _SRGB-format view is hardware-decoded at sample
// time), so only the authored tint (sRGB) is decoded before the multiply.

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

vec3 srgb_to_linear( vec3 c )
{
    bvec3 cutoff = lessThanEqual( c, vec3( 0.04045 ) );
    vec3  lo     = c / 12.92;
    vec3  hi     = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
    return mix( hi, lo, vec3( cutoff ) );
}

void main()
{
    vec4 s = texture( sampler2D( u_textures[nonuniformEXT( pc.tex_idx )],
                                 u_samplers[nonuniformEXT( pc.samp_idx )] ), v_uv );
    out_color = vec4( s.rgb * srgb_to_linear( v_color.rgb ), s.a * v_color.a );
}
