#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler   u_samplers[];

// The bindless storage-buffer array (set 0, binding 2).  The gui reads ONE table through it: the
// frame's clip entries, two vec4s each -- [0] = (x0, y0, x1, y1) pixel edges, [1].x = corner
// radius (see clip_coverage below).
layout(set = 0, binding = 2, std430) readonly buffer clip_buf_t { vec4 data[]; } u_buffers[];

layout(push_constant) uniform PC {
    mat4 mvp;
    uint samp_point;  // bindless sampler slot: NEAREST, for the coverage model
    uint samp_image;  // bindless sampler slot: LINEAR, for every model that filters
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    float time;      // effect-band frame clock, seconds wrapped to GUI_FX_TIME_WRAP (1024)
    uint clip_buf;   // bindless buffer slot of the frame's clip table (0 = no table, no clipping)
    uint clip_base;  // this draw's first entry in the clip table (entries, not vec4s)
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 2) in  vec2 v_fx_coord;
layout(location = 3) flat in uint v_fx;
layout(location = 4) flat in uint v_tex;   // sampling model (top 4 bits) | bindless slot
layout(location = 0) out vec4 out_color;

// Mirrors GUI_TEX_MODE_SHIFT / GUI_TEX_CLIP_SHIFT in gui.h -- keep the three in step.
#define TEX_MODE_SHIFT  28u
#define TEX_CLIP_SHIFT  22u
#define TEX_CLIP_MASK   0x3Fu
#define TEX_INDEX_MASK  0x003FFFFFu

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

    // modes 11 CHECKER / 12 GRID -- the framebuffer-tiling patterns (gui.h).  Taken before the
    // sector decode, whose `mode >= 7` gate would otherwise swallow them.  Both compute in
    // gl_FragCoord pixels: exact at any panel size, where the HALF2 effect coordinate's ulp
    // reaches a full pixel at the corners of a fullscreen backdrop.  The phase re-anchors the
    // pattern to the shape; the CPU derived it against the same quantized cell the word carries.
    if ( mode >= 11u )
    {
        if ( mode == 11u )
            return 1.0;   // CHECKER cuts nothing: it picks between two colours in main()

        // GRID: distance to the nearest lattice line on either axis, the line `thickness` px
        // wide straddling it, resolved with a 1 px AA band.  The mod is floor-based on purpose:
        // the phase-shifted coordinate can go negative near the origin.
        float cell = float( ( v_fx >>  4 ) & 0xFFFu ) * 0.25;
        float ht   = float( ( v_fx >> 16 ) & 0x7Fu  ) * 0.0625;   // HALF the line width
        vec2  p    = gl_FragCoord.xy - v_uv * cell;               // uv = per-axis phase fraction
        vec2  m    = p - cell * floor( p / cell );
        vec2  dl   = min( m, vec2( cell ) - m );
        return clamp( 0.5 + ht - min( dl.x, dl.y ), 0.0, 1.0 );
    }

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
            // Modes 9 (ARC_DASH) and 10 (ARC_GRAD) are this same band -- the dash cut is below,
            // the gradient acts on the COLOR in main() (the TEXT_EDGE precedent).
            ds = ( ( sc.y * q.x > sc.x * q.y ) ? length( q - sc * ra )
                                               : abs( length( q ) - ra ) ) - rb;
        }
        float cov = clamp( 0.5 - ds, 0.0, 1.0 );

        // mode 9 ARC_DASH -- the band cut by an angular dash pattern.  This mode is SELF-SAMPLED
        // (gui.h): the uv word carries (period / TAU, duty) instead of texcoords, flat across the
        // quad so it interpolates exactly.  theta is the SIGNED angle from the bisector (the same
        // signed coordinate the aperture cut uses); the pattern anchors at the sweep start, and
        // the emit side pre-quantized the period so whole cycles fit -- a closed ring has no seam.
        // The cut edge is antialiased in ARC-LENGTH pixels (angle error times ra).
        if ( mode == 9u )
        {
            float T     = max( v_uv.x, 1.0 / 65535.0 ) * 6.28318531;
            float duty  = v_uv.y;
            float t     = atan( v_fx_coord.x, v_fx_coord.y ) + ap;
            float m     = t - T * floor( t / T );
            float d_on  = min( m, duty * T - m );          // signed: > 0 inside an on-run
            cov = min( cov, clamp( 0.5 + d_on * ra, 0.0, 1.0 ) );
        }
        return cov;
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

// The clip band: which clip rect cuts this fragment, resolved HERE rather than by the hardware
// scissor so a clip change never opens a new draw call.  The vertex names its entry (bits 22..27
// of the tex word, local to the draw); pc.clip_base locates the draw's span inside the frame's
// clip table, which lives in a bindless storage buffer named by pc.clip_buf.  clip_buf 0 -- the
// reserved invalid slot -- means "no clip table bound": full coverage, so a pipeline user without
// a table (and the gui itself before its first upload) is never clipped by garbage.
//
// Entries are two vec4s: [0] = (x0, y0, x1, y1) pixel EDGES, pre-snapped CPU-side to the same
// grid the hardware scissor was, so radius-0 coverage is bit-identical to the scissor it
// replaces (fragment centres sit at .5 against integer edges; no AA band on purpose -- a clip is
// a cut, not a shape).  radius > 0 evaluates the rounded-box field with a 1 px AA band instead:
// the clip a scissor rect could never express.
float clip_coverage()
{
    if ( pc.clip_buf == 0u )
        return 1.0;
    uint  e   = ( pc.clip_base + ( ( v_tex >> TEX_CLIP_SHIFT ) & TEX_CLIP_MASK ) ) * 2u;
    vec4  r   = u_buffers[ pc.clip_buf ].data[ e ];
    float rad = u_buffers[ pc.clip_buf ].data[ e + 1u ].x;
    vec2  p   = gl_FragCoord.xy;
    if ( rad <= 0.0 )
        return ( p.x >= r.x && p.y >= r.y && p.x < r.z && p.y < r.w ) ? 1.0 : 0.0;
    vec2  c = ( r.xy + r.zw ) * 0.5;
    vec2  h = ( r.zw - r.xy ) * 0.5 - vec2( rad );
    vec2  q = abs( p - c ) - h;
    float d = min( max( q.x, q.y ), 0.0 ) + length( max( q, vec2( 0.0 ) ) ) - rad;
    return clamp( 0.5 - d, 0.0, 1.0 );
}

void main()
{
    // The clip cut applies to EVERYTHING, the debug views included -- the scissor this replaces
    // cut their geometry too, and a batch-view block that ignored its clip would lie about what
    // the draw painted.
    float ccov = clip_coverage();

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
                              float( (pc.dbg_tint >> 24 ) & 0xFFu ) / 255.0 * ccov );
            return;
        }
        out_color = vec4( v_color.rgb, ccov );  // wireframe keeps each window's own (linear) color
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
    float cov = fx_coverage() * ccov;   // every non-debug path below multiplies by cov

    // SELF-SAMPLED fx modes (9+): the shape is solid colour by definition, so the texel is not
    // consulted -- the uv word carried mode parameters instead (the sample above read a garbage
    // location of a VALID texture, which is harmless and cheaper than a divergent skip).  Mode 10
    // additionally sweeps the colour toward a second RGBA8 riding that word, lerped by the same
    // signed angle the aperture cut uses -- the gradient a 4-corner vertex colour cannot express.
    // Mode 11 picks between the vertex colour and that same second RGBA8 by cell parity.
    vec4 vcol = v_color;
    uint fxm  = v_fx & 0xFu;
    if ( fxm >= 9u )
    {
        s = vec4( 1.0 );
        if ( fxm == 10u )
        {
            uint  bu = uint( round( v_uv.x * 65535.0 ) );
            uint  bv = uint( round( v_uv.y * 65535.0 ) );
            vec4  c2 = vec4( float( bu & 0xFFu ), float( bu >> 8 ),
                             float( bv & 0xFFu ), float( bv >> 8 ) ) / 255.0;
            c2.rgb   = srgb_to_linear( c2.rgb );   // authored sRGB, never saw the vertex stage
            float ap = float( ( v_fx >> 23 ) & 0x1FFu ) * ( 3.14159265 / 511.0 );
            float t  = clamp( ( atan( v_fx_coord.x, v_fx_coord.y ) + ap )
                              / max( 2.0 * ap, 1e-4 ), 0.0, 1.0 );
            vcol = mix( vcol, c2, t );
        }
        // mode 11 CHECKER -- col_b decodes exactly as GRAD's does; the cell pitch and the parity
        // phase come from the effect word (phase is a fraction of the TWO-cell colour period, so
        // /255 * 2*cell = cell/127.5).  Parity is floor-based like every pattern mod here, and
        // odd cells take the second colour.
        else if ( fxm == 11u )
        {
            uint  bu = uint( round( v_uv.x * 65535.0 ) );
            uint  bv = uint( round( v_uv.y * 65535.0 ) );
            vec4  c2 = vec4( float( bu & 0xFFu ), float( bu >> 8 ),
                             float( bv & 0xFFu ), float( bv >> 8 ) ) / 255.0;
            c2.rgb     = srgb_to_linear( c2.rgb );   // authored sRGB, never saw the vertex stage
            float cell = float( ( v_fx >>  4 ) & 0xFFFu ) * 0.25;
            vec2  ph   = vec2( float( ( v_fx >> 16 ) & 0xFFu ),
                               float( ( v_fx >> 24 ) & 0xFFu ) ) * ( cell / 127.5 );
            vec2  t    = ( gl_FragCoord.xy - ph ) / cell;
            float k    = floor( t.x ) + floor( t.y );
            if ( k - 2.0 * floor( k * 0.5 ) >= 0.5 )
                vcol = c2;
        }
    }

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

    // vcol.rgb is already linear light (v_color, or the mode-10 sweep of it); alpha is coverage,
    // which was linear all along.  s.r is the glyph coverage from the R8 atlas (1.0 for the white
    // solid-color pixel and for the self-sampled modes, so non-text draws pass through).
    out_color = vec4( vcol.rgb, vcol.a * s.r * cov );
}
