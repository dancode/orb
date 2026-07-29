#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler   u_samplers[];

layout(push_constant) uniform PC {
    mat4 mvp;
    uint samp_point;  // bindless sampler slot: NEAREST, for the coverage model
    uint samp_image;  // bindless sampler slot: LINEAR, for every model that filters
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    float time;      // effect-band frame clock, seconds wrapped to GUI_FX_TIME_WRAP (1024)
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 2) in  vec2 v_fx_coord;
layout(location = 3) flat in uint v_fx;
layout(location = 4) flat in uint v_tex;   // sampling model (top 4 bits) | bindless slot
layout(location = 0) out vec4 out_color;

// Mirrors GUI_TEX_MODE_SHIFT / GUI_TEX_MODE_MASK in gui.h -- keep the three in step.
#define TEX_MODE_SHIFT  28u
#define TEX_INDEX_MASK  0x0FFFFFFFu

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
    // 0 NONE, and the two modes that are not shapes: 4 TILE_U acted in the vertex stage, 5
    // TEXT_EDGE acts on the COLOR in the SDF branch below.  All three contribute full coverage --
    // the band names what the fragment does, and "nothing, here" is a legal answer.
    if ( mode == 0u || mode == 4u || mode == 5u )
        return 1.0;

    // modes 7 ARC and 8 PIE -- circular sectors.  Taken before the shared decode below because they
    // re-partition the word completely: there is no feather field (a sector gets a fixed 1 px band,
    // which is what `0.5 - d` is), and the 9 bits it would have cost carry the APERTURE instead.
    //
    // v_fx_coord is SIGNED here, not folded, and already rotated by the CPU so the sector's bisector
    // points +y (gui.h).  That is the whole trick: a circular shape subtracts no half-extent, so its
    // coordinate is affine over one quad and the sign survives -- and the sign is the angle, without
    // which neither of these shapes can be expressed at all.  The fold below is the fragment's own,
    // exact because the value it folds is exact.
    if ( mode >= 7u )
    {
        float ra = float( ( v_fx >>  4 ) & 0xFFFu ) * 0.125;
        float rb = float( ( v_fx >> 16 ) & 0x7Fu  ) * 0.125;
        float ap = float( ( v_fx >> 23 ) & 0x1FFu ) * ( 3.14159265 / 511.0 );
        vec2  sc = vec2( sin( ap ), cos( ap ) );   // the aperture as a direction, once per fragment
        vec2  q  = vec2( abs( v_fx_coord.x ), v_fx_coord.y );
        float ds;

        if ( mode == 8u )
        {
            // PIE: the disc intersected with the angular wedge.  `l` is the disc, `m` the distance
            // to the radial edge (a segment from the centre out to the rim, hence the clamp to ra),
            // and the cross product's sign says which side of that edge we are on -- so max() keeps
            // the edges SHARP where the arc's round caps would have rounded them.
            float l = length( q ) - ra;
            float m = length( q - sc * clamp( dot( q, sc ), 0.0, ra ) );
            ds = max( l, m * sign( sc.y * q.x - sc.x * q.y ) );
        }
        else
        {
            // ARC: exact distance to the circle of radius ra, cut to the aperture, then thickened
            // by the tube.  Inside the wedge it is the annulus; outside it is the distance to the
            // nearest endpoint, which is what gives the stroke its round caps for free.
            ds = ( ( sc.y * q.x > sc.x * q.y ) ? length( q - sc * ra )
                                               : abs( length( q ) - ra ) ) - rb;
        }
        return clamp( 0.5 - ds, 0.0, 1.0 );
    }

    float radius  = float( ( v_fx >>  4 ) & 0xFFFu ) * 0.125;
    float feather = float( ( v_fx >> 16 ) & 0x1FFu ) * 0.25;
    float border  = float( ( v_fx >> 25 ) & 0x7Fu  ) * 0.125;

    vec2  q = v_fx_coord;
    float d;

    // mode 6 SEG -- a CAPSULE: the distance to a line segment, minus its half-thickness.  q.x is
    // |along| - halflen and q.y is the SIGNED across-axis offset, which needs no fold because
    // length() squares it.  That asymmetry is the point: the box folds both axes at the vertex
    // because both subtract a half-extent, so it costs four quadrant quads; the segment subtracts
    // on one axis only and costs two.  No interior term either -- this form is already the exact
    // signed distance in the core, where the rounded box's length-only form saturates.
    if ( mode == 6u )
    {
        d = length( vec2( max( q.x, 0.0 ), q.y ) ) - radius;
    }
    else
    {
        d = min( max( q.x, q.y ), 0.0 ) + length( max( q, vec2( 0.0 ) ) ) - radius;
        if ( mode == 2u )
            d = abs( d + border * 0.5 ) - border * 0.5;
    }

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

    /* The texture and its sampling model arrive PER VERTEX (gui.h), so one draw call can mix a
       coverage atlas, an SDF atlas and arbitrary RGBA images.  nonuniformEXT is mandatory here and
       not a formality: neighbouring primitives in a single draw now legitimately name different
       descriptors, so the index is not wave-uniform and indexing without it is undefined.
       The SAMPLER is derived from the model rather than carried -- coverage must stay point-sampled
       (a filtered glyph atlas stops being crisp) and every other model filters.  Testing "not
       COVERAGE" means a model added later lands on the filtering side without touching this. */
    uint  tex_mode = v_tex >> TEX_MODE_SHIFT;
    uint  tex_slot = v_tex & TEX_INDEX_MASK;
    uint  samp     = ( tex_mode == 0u ) ? pc.samp_point : pc.samp_image;
    vec4  s        = texture( sampler2D( u_textures[nonuniformEXT( tex_slot )],
                                         u_samplers[nonuniformEXT( samp )] ), v_uv );
    float cov = fx_coverage();

    // GUI_TEX_RGBA -- full-RGBA image (scene viewport / arbitrary bindless texture): the texel IS
    // the color, with the vertex color acting as a tint.  The texel arrives LINEAR: _SRGB-format
    // textures are hardware-decoded at sample time, and UNORM render targets hold linear data.
    // The tint is linear too (decoded in the vertex stage), so both sides of this multiply are
    // light and the product means something.
    // Compared against the exact model rather than "non-zero" so a model added later falls through
    // to the coverage path only if that is what it actually wants.
    if ( tex_mode == 1u )
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
        if ( ( v_fx & 0xFu ) == 5u )
        {
            float wpx = float( ( v_fx >>  4 ) & 0xFFu ) * 0.125;
            float ea  = float( ( v_fx >> 27 ) & 0x1Fu ) / 31.0;
            // The edge colour is authored sRGB and rides the effect word, so like the debug tint it
            // never passed through the vertex stage and is decoded here.  That cost lands only on
            // outlined glyphs, which is why it is not hoisted.
            vec3  ergb = srgb_to_linear( vec3( float( ( v_fx >> 12 ) & 0x1Fu ),
                                               float( ( v_fx >> 17 ) & 0x1Fu ),
                                               float( ( v_fx >> 22 ) & 0x1Fu ) ) / 31.0 );

            // Source-over of the fill onto the band, resolved analytically: the band contributes
            // only where the fill does not (1 - fill), so the seam between them is antialiased by
            // the same coverage that antialiases the glyph, and the two never double-darken.
            float outer = clamp( dpx + wpx + 0.5, 0.0, 1.0 );
            float af    = v_color.a * fill;
            float ao    = ea * outer * ( 1.0 - fill );
            float at    = af + ao;
            out_color   = vec4( ( v_color.rgb * af + ergb * ao ) / max( at, 1e-6 ), at * cov );
            return;
        }

        out_color = vec4( v_color.rgb, v_color.a * cov * fill );
        return;
    }

    // v_color.rgb is already linear light; alpha is coverage, which was linear all along. s.r is
    // the glyph coverage from the R8 atlas (1.0 for the white solid-color pixel, so non-text draws
    // pass through).
    out_color = vec4( v_color.rgb, v_color.a * s.r * cov );
}
