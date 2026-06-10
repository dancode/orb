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

// Decode an sRGB-encoded color to linear light. UI colors are authored in sRGB
// (the values you type as hex / pick in a color picker), but the swapchain is a
// _SRGB format, so the GPU blends in linear space and re-encodes on store. The
// vertex color arrives as raw UNORM bytes (no automatic decode), so we linearize
// it here to keep alpha blending physically correct -- the normal game-engine path.
vec3 srgb_to_linear( vec3 c )
{
    bvec3 cutoff = lessThanEqual( c, vec3( 0.04045 ) );
    vec3  lo     = c / 12.92;
    vec3  hi     = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
    return mix( hi, lo, vec3( cutoff ) );
}

void main()
{
    vec4 s = texture( sampler2D( u_textures[pc.tex_idx], u_samplers[pc.samp_idx] ), v_uv );
    // Only RGB is gamma-decoded; alpha is linear coverage. s.r is the glyph coverage
    // from the R8 atlas (1.0 for the white solid-color pixel, so non-text draws pass through).
    out_color = vec4( srgb_to_linear( v_color.rgb ), v_color.a * s.r );
}
