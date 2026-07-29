// gui.ps.hlsl -- HLSL twin of gui.frag (the cooked-shader path).
//
// Cooked into bin/shaders/gui.ps.oshd; see gui.vs.hlsl for the cooked-vs-fallback contract.
// Keep the push constant block identical to the vertex stage and to gui_push_t.

struct gui_pc_t
{
    float4x4 mvp;          // column-major pixel-space ortho (Vulkan clip space)
    uint     samp_point;   // bindless sampler slot: NEAREST, for the coverage model
    uint     samp_image;   // bindless sampler slot: LINEAR, for every model that filters
    uint     dbg_flat;     // debug: 1 = ignore atlas coverage, output a flat color
    uint     dbg_tint;     // debug: packed RGBA8 batch tint (0 = use vertex color)
    float    time;         // effect-band frame clock, wrapped seconds (GUI_FX_TIME_WRAP)
};
[[vk::push_constant]] gui_pc_t pc;

[[vk::binding( 0, 0 )]] Texture2D    u_textures[] : register( t0, space0 );
[[vk::binding( 1, 0 )]] SamplerState u_samplers[] : register( s0, space0 );

// Mirrors GUI_TEX_MODE_SHIFT / GUI_TEX_MODE_MASK in gui.h -- keep the three in step.
#define TEX_MODE_SHIFT  30u
#define TEX_INDEX_MASK  0x3FFFFFFFu

struct ps_in_t
{
    float4                  sv_pos   : SV_Position;
    float4                  color    : COLOR0;
    float2                  uv       : TEXCOORD0;
    float2                  fx_coord : TEXCOORD1;
    nointerpolation uint    fx       : TEXCOORD2;
    nointerpolation uint    tex      : TEXCOORD3;
};

// The effect band (gui.h): coverage of the shape this fragment's vertex named, 1.0 when it named
// none.  fx_coord is |p| - c, so the corner arc the CPU used to tessellate is simply where both
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
// looks like: frame-constant, so it costs no vertex change, no re-emit and no batch split.
float fx_coverage( float2 fx_coord, uint fx )
{
    uint mode = fx & 0xFu;
    // 0 NONE, and the two modes that are not shapes: 4 TILE_U acted in the vertex stage, 5
    // TEXT_EDGE acts on the COLOR in the SDF branch below.  All three contribute full coverage --
    // the band names what the fragment does, and "nothing, here" is a legal answer.
    if ( mode == 0u || mode == 4u || mode == 5u )
        return 1.0;

    float radius  = float( ( fx >>  4 ) & 0xFFFu ) * 0.125;
    float feather = float( ( fx >> 16 ) & 0x1FFu ) * 0.25;
    float border  = float( ( fx >> 25 ) & 0x7Fu  ) * 0.125;

    float2 q = fx_coord;
    float  d;

    // mode 6 SEG -- a CAPSULE: the distance to a line segment, minus its half-thickness.  q.x is
    // |along| - halflen and q.y is the SIGNED across-axis offset, which needs no fold because
    // length() squares it.  That asymmetry is the point: the box folds both axes at the vertex
    // because both subtract a half-extent, so it costs four quadrant quads; the segment subtracts
    // on one axis only and costs two.  No interior term either -- this form is already the exact
    // signed distance in the core, where the rounded box's length-only form saturates.
    if ( mode == 6u )
    {
        d = length( float2( max( q.x, 0.0 ), q.y ) ) - radius;
    }
    else
    {
        d = min( max( q.x, q.y ), 0.0 ) + length( max( q, float2( 0.0, 0.0 ) ) ) - radius;
        if ( mode == 2u )
            d = abs( d + border * 0.5 ) - border * 0.5;
    }

    float cov = ( feather <= 0.0 ) ? ( d <= 0.0 ? 1.0 : 0.0 )
                                   : saturate( 0.5 - d / feather );

    // PULSE reuses the same two shifts for radius/feather and reads the top 7 bits as rate+depth.
    // The wave starts at its PEAK (cos 0 = 1 -> no attenuation), so a pulse fading in from nothing
    // is never what the first frame shows.
    if ( mode == 3u )
    {
        float rate  = float( ( fx >> 25 ) & 0xFu ) * 0.25;
        float depth = float( ( fx >> 29 ) & 0x7u ) / 7.0;
        cov *= 1.0 - depth * ( 0.5 - 0.5 * cos( 6.28318531 * rate * pc.time ) );
    }

    return cov;
}

// i.color arrives ALREADY LINEAR -- the vertex stage decodes it (see gui.vs.hlsl), because it is a
// per-vertex constant and decoding it per fragment spent three pow() on every pixel of the UI.
// Nothing below this line linearizes the vertex color; doing so would decode it twice.
//
// This function survives for the one color that does NOT come down the pipe: the debug batch tint,
// which is authored sRGB in a push constant and read only by the debug view below.
float3 srgb_to_linear( float3 c )
{
    float3 lo = c / 12.92;
    float3 hi = pow( ( c + 0.055 ) / 1.055, 2.4 );
    return lerp( hi, lo, step( c, 0.04045 ) );    // c <= 0.04045 selects lo (GLSL mix + cutoff)
}

float4 main( ps_in_t i ) : SV_Target0
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
            float3 rgb = float3( float(   pc.dbg_tint         & 0xFFu ),
                                 float( ( pc.dbg_tint >> 8  ) & 0xFFu ),
                                 float( ( pc.dbg_tint >> 16 ) & 0xFFu ) ) / 255.0;
            return float4( srgb_to_linear( rgb ),
                           float( ( pc.dbg_tint >> 24 ) & 0xFFu ) / 255.0 );
        }
        return float4( i.color.rgb, 1.0 );    // wireframe keeps each window's own (linear) color
    }

    /* The texture and its sampling model arrive PER VERTEX (gui.h), so one draw call can mix a
       coverage atlas, an SDF atlas and arbitrary RGBA images.  NonUniformResourceIndex is mandatory
       here and not a formality: neighbouring primitives in a single draw now legitimately name
       different descriptors, so the index is not wave-uniform and indexing without it is undefined.
       The SAMPLER is derived from the model rather than carried -- coverage must stay point-sampled
       (a filtered glyph atlas stops being crisp) and every other model filters.  Testing "not
       COVERAGE" means a model added later lands on the filtering side without touching this. */
    uint   tex_mode = i.tex >> TEX_MODE_SHIFT;
    uint   tex_slot = i.tex & TEX_INDEX_MASK;
    uint   samp     = ( tex_mode == 0u ) ? pc.samp_point : pc.samp_image;
    float4 s        = u_textures[ NonUniformResourceIndex( tex_slot ) ]
                          .Sample( u_samplers[ NonUniformResourceIndex( samp ) ], i.uv );
    float  cov = fx_coverage( i.fx_coord, i.fx );

    // GUI_TEX_RGBA -- full-RGBA image (scene viewport / arbitrary bindless texture): the texel IS
    // the color, with the vertex color acting as a tint.  The texel arrives LINEAR: _SRGB-format
    // textures are hardware-decoded at sample time, and UNORM render targets hold linear data.
    // The tint is linear too (decoded in the vertex stage), so both sides of this multiply are
    // light and the product means something.
    // Compared against the exact model rather than "non-zero" so a model added later falls through
    // to the coverage path only if that is what it actually wants.
    if ( tex_mode == 1u )
        return float4( s.rgb * i.color.rgb, s.a * i.color.a * cov );

    // GUI_TEX_SDF -- distance-field text.  The texel is not coverage: 128/255 is exactly ON the
    // outline, above is inside, below is outside (orb_font.h).  Coverage is recovered from the
    // SCREEN-SPACE DERIVATIVE of that field, which is the whole reason this mode exists:
    // fwidth(d) is how much the distance changes across one pixel HERE, so d/fwidth(d) is the
    // distance to the edge measured in pixels no matter how the quad was scaled or rotated.  The
    // AA band is therefore always one pixel wide, and no per-vertex or per-draw parameter has to
    // carry the scale -- which is why an SDF font costs the vertex format nothing.
    // The max() guards the degenerate case: deep inside or far outside the field is flat, fwidth
    // is 0, and d/0 would be a NaN rather than the saturated 1 or 0 that is wanted there.
    if ( tex_mode == 2u )
    {
        float d    = s.r - ( 128.0 / 255.0 );
        float dpx  = d / max( fwidth( d ), 1e-6 );        // signed distance to the edge, in PIXELS
        float fill = clamp( dpx + 0.5, 0.0, 1.0 );

        // GUI_FX_TEXT_EDGE -- a second colour outside the glyph boundary (Slate's SecondaryColor,
        // without Slate's vertex field).  Because dpx is already a pixel distance, "outline" is
        // just the same threshold moved out by `width`: one extra clamp on a number this branch had
        // to compute anyway, from the ONE texture sample it had already taken.  No second quad, no
        // offset copy of the run, no batch split -- which is what makes it affordable on body text.
        if ( ( i.fx & 0xFu ) == 5u )
        {
            float wpx = float( ( i.fx >>  4 ) & 0xFFu ) * 0.125;
            float ea  = float( ( i.fx >> 27 ) & 0x1Fu ) / 31.0;
            // The edge colour is authored sRGB and rides the effect word, so like the debug tint it
            // never passed through the vertex stage and is decoded here.  That cost lands only on
            // outlined glyphs, which is why it is not hoisted.
            float3 ergb = srgb_to_linear( float3( float( ( i.fx >> 12 ) & 0x1Fu ),
                                                  float( ( i.fx >> 17 ) & 0x1Fu ),
                                                  float( ( i.fx >> 22 ) & 0x1Fu ) ) / 31.0 );

            // Source-over of the fill onto the band, resolved analytically: the band contributes
            // only where the fill does not (1 - fill), so the seam between them is antialiased by
            // the same coverage that antialiases the glyph, and the two never double-darken.
            float outer = clamp( dpx + wpx + 0.5, 0.0, 1.0 );
            float af    = i.color.a * fill;
            float ao    = ea * outer * ( 1.0 - fill );
            float at    = af + ao;
            return float4( ( i.color.rgb * af + ergb * ao ) / max( at, 1e-6 ), at * cov );
        }

        return float4( i.color.rgb, i.color.a * cov * fill );
    }

    // i.color.rgb is already linear light; alpha is coverage, which was linear all along.  s.r is
    // the glyph coverage from the R8 atlas (1.0 for the white solid-color pixel, so non-text draws
    // pass through).
    return float4( i.color.rgb, i.color.a * s.r * cov );
}
