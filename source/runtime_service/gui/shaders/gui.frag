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
    uint rgba_tex;   // 1 = sample tex_idx as a full RGBA image (scene viewport), not R8 coverage
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 2) in  vec2 v_fx_coord;
layout(location = 3) flat in uint v_fx;
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

// The effect band (gui.h): coverage of the shape this fragment's vertex named, 1.0 when it named
// none.  v_fx_coord is |p| - c, so the corner arc the CPU used to tessellate is simply where both
// components go positive at once.
//   mode 1 BOX  -- fill inside the boundary
//   mode 2 RING -- the band of `border` px lying inside the boundary
// feather is the total width of the transition, straddling the boundary, so coverage is 0.5
// exactly on it; feather 0 means a hard edge (no antialiasing).
//
// The min(max(q.x,q.y),0) term is the INTERIOR distance and it is not optional: without it the
// field saturates at -radius everywhere inside, which silently breaks the two cases that need
// depth rather than proximity -- a border wider than the radius (the whole interior lands in the
// band and fills), and a shadow whose falloff is wider than the radius (its core never reaches
// full opacity).
float fx_coverage()
{
    uint mode = v_fx & 0xFu;
    if ( mode == 0u )
        return 1.0;

    float radius  = float( ( v_fx >>  4 ) & 0xFFFu ) * 0.125;
    float feather = float( ( v_fx >> 16 ) & 0x1FFu ) * 0.25;
    float border  = float( ( v_fx >> 25 ) & 0x7Fu  ) * 0.125;

    vec2  q = v_fx_coord;
    float d = min( max( q.x, q.y ), 0.0 ) + length( max( q, vec2( 0.0 ) ) ) - radius;
    if ( mode == 2u )
        d = abs( d + border * 0.5 ) - border * 0.5;

    if ( feather <= 0.0 )
        return d <= 0.0 ? 1.0 : 0.0;
    return clamp( 0.5 - d / feather, 0.0, 1.0 );
}

void main()
{
    // Debug views: bypass the atlas so geometry is visible regardless of glyph coverage.
    //   wireframe -- the LINE pipeline strokes triangle edges; a flat opaque color makes them
    //                show even across text quads (where s.r would otherwise alpha them away).
    //   batch     -- each draw call is pushed a distinct dbg_tint so its geometry reads as one
    //                solid color block; a color change marks a batch split.
    // The effect band is bypassed here too, on purpose: these views exist to show the geometry
    // actually submitted, and an SDF surface's four quadrant quads are exactly what you want to
    // see when asking why a shape looks the way it does.
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

    vec4  s   = texture( sampler2D( u_textures[pc.tex_idx], u_samplers[pc.samp_idx] ), v_uv );
    float cov = fx_coverage();

    // Full-RGBA image path (scene viewport / arbitrary bindless texture): the texel IS the
    // color, with the vertex color acting as a tint.  The texel arrives LINEAR: _SRGB-format
    // textures are hardware-decoded at sample time, and UNORM render targets hold linear data.
    // Only the authored tint color needs the sRGB decode.
    if ( pc.rgba_tex != 0u )
    {
        out_color = vec4( s.rgb * srgb_to_linear( v_color.rgb ), s.a * v_color.a * cov );
        return;
    }

    // Only RGB is gamma-decoded; alpha is linear coverage. s.r is the glyph coverage
    // from the R8 atlas (1.0 for the white solid-color pixel, so non-text draws pass through).
    out_color = vec4( srgb_to_linear( v_color.rgb ), v_color.a * s.r * cov );
}
