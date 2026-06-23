#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler   u_samplers[];

layout(push_constant) uniform PC {
    mat4 mvp;
    uint tex_idx;
    uint samp_idx;
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
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
    // Debug views: bypass the atlas so geometry is visible regardless of glyph coverage.
    //   wireframe -- the LINE pipeline strokes triangle edges; a flat opaque color makes them
    //                show even across text quads (where s.r would otherwise alpha them away).
    //   batch     -- each draw call is pushed a distinct dbg_tint so its geometry reads as one
    //                solid color block; a color change marks a batch split.
    if ( pc.dbg_flat != 0u )
    {
        vec3  rgb;
        float a;
        if ( pc.dbg_tint != 0u )
        {
            rgb = vec3( float(  pc.dbg_tint        & 0xFFu ),
                        float( (pc.dbg_tint >> 8 )  & 0xFFu ),
                        float( (pc.dbg_tint >> 16 ) & 0xFFu ) ) / 255.0;
            a   = float( (pc.dbg_tint >> 24 ) & 0xFFu ) / 255.0;
        }
        else
        {
            rgb = v_color.rgb;   // wireframe keeps each window's own color
            a   = 1.0;
        }
        out_color = vec4( srgb_to_linear( rgb ), a );
        return;
    }

    vec4 s = texture( sampler2D( u_textures[pc.tex_idx], u_samplers[pc.samp_idx] ), v_uv );
    // Only RGB is gamma-decoded; alpha is linear coverage. s.r is the glyph coverage
    // from the R8 atlas (1.0 for the white solid-color pixel, so non-text draws pass through).
    out_color = vec4( srgb_to_linear( v_color.rgb ), v_color.a * s.r );
}
