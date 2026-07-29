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
    uint tex_mode;   // sampling model (gui_tex_mode_t): 0 = R8 coverage, 1 = full RGBA image
    float time;      // effect-band frame clock, seconds wrapped to GUI_FX_TIME_WRAP (1024)
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 2) in  vec2 v_fx_coord;
layout(location = 3) flat in uint v_fx;
layout(location = 0) out vec4 out_color;

// v_color arrives ALREADY LINEAR -- the vertex stage decodes it (see gui.vert), because it is a
// per-vertex constant and decoding it per fragment spent three pow() on every pixel of the UI.
// Nothing below this line linearizes the vertex color; doing so would decode it twice.
//
// This function survives for the one color that does NOT come down the pipe: the debug batch tint,
// which is authored sRGB in a push constant and read only by the debug view below.
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
//   mode 1 BOX   -- fill inside the boundary
//   mode 2 RING  -- the band of `border` px lying inside the boundary
//   mode 3 PULSE -- BOX, alpha breathing on pc.time (rate + depth replace border in the word)
// feather is the total width of the transition, straddling the boundary, so coverage is 0.5
// exactly on it; feather 0 means a hard edge (no antialiasing).
//
// The min(max(q.x,q.y),0) term is the INTERIOR distance and it is not optional: without it the
// field saturates at -radius everywhere inside, which silently breaks the two cases that need
// depth rather than proximity -- a border wider than the radius (the whole interior lands in the
// band and fills), and a shadow whose falloff is wider than the radius (its core never reaches
// full opacity).
//
// pc.time -- the wrapped frame clock -- is the band's animation seam, and PULSE is what reading it
// looks like: frame-constant, so it costs no vertex change, no re-emit and no batch split.  The fx
// word is fully packed, so a time-driven mode re-partitions the 28 parameter bits it already has
// rather than asking for more.
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

    float cov = ( feather <= 0.0 ) ? ( d <= 0.0 ? 1.0 : 0.0 )
                                   : clamp( 0.5 - d / feather, 0.0, 1.0 );

    // PULSE reuses the same two shifts for radius/feather and reads the top 7 bits as rate+depth.
    // The wave starts at its PEAK (cos 0 = 1 -> no attenuation), so a pulse fading in from nothing
    // is never what the first frame shows.
    if ( mode == 3u )
    {
        float rate  = float( ( v_fx >> 25 ) & 0xFu ) * 0.25;
        float depth = float( ( v_fx >> 29 ) & 0x7u ) / 7.0;
        cov *= 1.0 - depth * ( 0.5 - 0.5 * cos( 6.28318531 * rate * pc.time ) );
    }

    return cov;
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
        if ( pc.dbg_tint != 0u )
        {
            // The tint is authored sRGB and rides the push constant, so it is the one color that
            // still needs decoding here -- it never went through the vertex stage.
            vec3 rgb = vec3( float(  pc.dbg_tint        & 0xFFu ),
                             float( (pc.dbg_tint >> 8 )  & 0xFFu ),
                             float( (pc.dbg_tint >> 16 ) & 0xFFu ) ) / 255.0;
            out_color = vec4( srgb_to_linear( rgb ),
                              float( (pc.dbg_tint >> 24 ) & 0xFFu ) / 255.0 );
            return;
        }
        out_color = vec4( v_color.rgb, 1.0 );   // wireframe keeps each window's own (linear) color
        return;
    }

    vec4  s   = texture( sampler2D( u_textures[pc.tex_idx], u_samplers[pc.samp_idx] ), v_uv );
    float cov = fx_coverage();

    // GUI_TEX_RGBA -- full-RGBA image (scene viewport / arbitrary bindless texture): the texel IS
    // the color, with the vertex color acting as a tint.  The texel arrives LINEAR: _SRGB-format
    // textures are hardware-decoded at sample time, and UNORM render targets hold linear data.
    // The tint is linear too (decoded in the vertex stage), so both sides of this multiply are
    // light and the product means something.
    // Compared against the exact model rather than "non-zero" so a model added later falls through
    // to the coverage path only if that is what it actually wants.
    if ( pc.tex_mode == 1u )
    {
        out_color = vec4( s.rgb * v_color.rgb, s.a * v_color.a * cov );
        return;
    }

    // GUI_TEX_SDF -- distance-field text.  The texel is not coverage: 128/255 is exactly ON the
    // outline, above is inside, below is outside (orb_font.h).  Coverage is recovered from the
    // SCREEN-SPACE DERIVATIVE of that field, which is the whole reason this mode exists:
    // fwidth(d) is how much the distance changes across one pixel HERE, so d/fwidth(d) is the
    // distance to the edge measured in pixels no matter how the quad was scaled or rotated. The
    // AA band is therefore always one pixel wide, and no per-vertex or per-draw parameter has to
    // carry the scale -- which is why an SDF font costs the vertex format nothing.
    // The max() guards the degenerate case: deep inside or far outside the field is flat, fwidth
    // is 0, and d/0 would be a NaN rather than the saturated 1 or 0 that is wanted there.
    if ( pc.tex_mode == 2u )
    {
        float d = s.r - ( 128.0 / 255.0 );
        out_color = vec4( v_color.rgb,
                          v_color.a * cov * clamp( d / max( fwidth( d ), 1e-6 ) + 0.5, 0.0, 1.0 ) );
        return;
    }

    // v_color.rgb is already linear light; alpha is coverage, which was linear all along. s.r is
    // the glyph coverage from the R8 atlas (1.0 for the white solid-color pixel, so non-text draws
    // pass through).
    out_color = vec4( v_color.rgb, v_color.a * s.r * cov );
}
