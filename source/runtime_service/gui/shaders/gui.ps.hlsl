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
    uint     clip_buf;     // bindless buffer slot of the frame's clip table (0 = no clipping)
    uint     clip_base;    // the flush's clip-region origin in the table (entries, not float4s)
};
[[vk::push_constant]] gui_pc_t pc;

[[vk::binding( 0, 0 )]] Texture2D    u_textures[] : register( t0, space0 );
[[vk::binding( 1, 0 )]] SamplerState u_samplers[] : register( s0, space0 );

// The bindless storage-buffer array (set 0, binding 2).  The gui reads ONE table through it: the
// frame's clip entries, two float4s each -- [0] = (x0, y0, x1, y1) pixel edges, [1].x = corner
// radius (see clip_coverage below).
[[vk::binding( 2, 0 )]] StructuredBuffer<float4> u_buffers[] : register( t0, space1 );

// Mirrors the tex-word fields in gui.h -- model, clip band, self bit and op band.  Keep gui.h,
// gui.frag and gui.ps.hlsl in step, then resplice the SPIR-V and re-cook.
#define TEX_MODE_SHIFT  28u
#define TEX_CLIP_SHIFT  19u
#define TEX_CLIP_MASK   0x1FFu
#define TEX_SELF_BIT    0x00040000u
#define TEX_OP_BAND     0x00001000u
#define TEX_OP_CUT      0x00002000u
#define TEX_OP_INSET    0x00004000u
#define TEX_OP_PULSE    0x00008000u
#define TEX_INDEX_MASK  0x00000FFFu

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
//   mode 1 BOX -- fill inside the boundary
//   mode 6 SEG -- a capsule (below)
// Those are the two shapes that reach the shared decode at the bottom, and the OP BAND in the tex
// word modifies whichever one arrived: BAND bends the field into a border, CUT and INSET cut the
// coverage against the boundary in opposite directions, PULSE breathes it on pc.time.  Every op
// therefore applies to both shapes -- a stroked capsule needs no mode of its own.
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
float fx_coverage( float2 fx_coord, uint fx, uint tex, float2 uv, float2 px )
{
    uint mode = fx & 0xFu;
    // 0 NONE, and the two modes that are not shapes: 4 TILE_U acted in the vertex stage, 5
    // TEXT_EDGE acts on the COLOR in the SDF branch below.  All three contribute full coverage --
    // the band names what the fragment does, and "nothing, here" is a legal answer.
    if ( mode == 0u || mode == 4u || mode == 5u )
        return 1.0;

    // modes 11 CHECKER / 12 GRID -- the framebuffer-tiling patterns (gui.h).  Taken before the
    // sector decode, whose `mode >= 7` gate would otherwise swallow them.  Both compute in
    // SV_Position pixels: exact at any panel size, where the HALF2 effect coordinate's ulp
    // reaches a full pixel at the corners of a fullscreen backdrop.  The phase re-anchors the
    // pattern to the shape; the CPU derived it against the same quantized cell the word carries.
    if ( mode == 11u || mode == 12u )
    {
        if ( mode == 11u )
            return 1.0;   // CHECKER cuts nothing: it picks between two colours in main()

        // GRID: distance to the nearest lattice line, the line `thickness` px wide straddling
        // it, resolved with a 1 px AA band.  The mod is floor-based on purpose: fmod truncates,
        // and the phase-shifted coordinate can go negative near the origin.
        float  cell = float( ( fx >>  4 ) & 0xFFFu ) * 0.25;
        float  ht   = float( ( fx >> 16 ) & 0x7Fu  ) * 0.0625;   // HALF the line width
        float  ang  = float( ( fx >> 23 ) & 0xFFu  ) * ( 3.14159265 / 255.0 );

        // Into LATTICE space first, so everything below is the axis-aligned case it always was.
        // The rotation is of the pixel coordinate, not of the pattern: the phase the CPU sent was
        // computed against the ROTATED anchor for exactly this reason (tess_grid), so the lines
        // still land on the anchor after the turn instead of sliding off it.
        float2 q = px;
        if ( ang != 0.0 )
        {
            float cs = cos( ang ), sn = sin( ang );
            q = float2( px.x * cs + px.y * sn, -px.x * sn + px.y * cs );
        }

        float2 p  = q - uv * cell;                               // uv = per-axis phase fraction
        float2 m  = p - cell * floor( p / cell );
        float2 dl = min( m, float2( cell, cell ) - m );

        // Bit 31 cuts on ONE axis instead of both: a lattice becomes a stripe field, and a stripe
        // field at an angle is the diagonal hatch.  Same quad, same cost.
        float d = ( ( fx & 0x80000000u ) != 0u ) ? dl.x : min( dl.x, dl.y );
        return saturate( 0.5 + ht - d );
    }

    // modes 7 ARC and 8 PIE -- circular sectors.  Taken before the shared decode below because they
    // re-partition the word completely: there is no feather field (a sector gets a fixed 1 px band,
    // which is what `0.5 - d` is), and the 9 bits it would have cost carry the APERTURE instead.
    //
    // fx_coord is SIGNED here, not folded, and already rotated by the CPU so the sector's bisector
    // points +y (gui.h).  That is the whole trick: a circular shape subtracts no half-extent, so its
    // coordinate is affine over one quad and the sign survives -- and the sign is the angle, without
    // which neither of these shapes can be expressed at all.  The fold below is the fragment's own,
    // exact because the value it folds is exact.
    if ( mode >= 7u && mode <= 10u )
    {
        float  ra = float( ( fx >>  4 ) & 0xFFFu ) * 0.125;
        float  rb = float( ( fx >> 16 ) & 0x7Fu  ) * 0.125;
        float  ap = float( ( fx >> 23 ) & 0x1FFu ) * ( 3.14159265 / 511.0 );
        float2 sc = float2( sin( ap ), cos( ap ) );  // the aperture as a direction, once per fragment
        float2 q  = float2( abs( fx_coord.x ), fx_coord.y );
        float  ds;

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
        float cov = saturate( 0.5 - ds );

        // mode 9 ARC_DASH -- the band cut by an angular dash pattern.  This mode is SELF-SAMPLED
        // (gui.h): the uv word carries (period / TAU, duty) instead of texcoords, flat across the
        // quad so it interpolates exactly.  theta is the SIGNED angle from the bisector (the same
        // signed coordinate the aperture cut uses); the pattern anchors at the sweep start, and
        // the emit side pre-quantized the period so whole cycles fit -- a closed ring has no seam.
        // The cut edge is antialiased in ARC-LENGTH pixels (angle error times ra).  The mod is
        // floor-based on purpose: fmod truncates and goes wrong for the cap's small negatives.
        if ( mode == 9u )
        {
            float T    = max( uv.x, 1.0 / 65535.0 ) * 6.28318531;
            float duty = uv.y;
            float t    = atan2( fx_coord.x, fx_coord.y ) + ap;
            float m    = t - T * floor( t / T );
            float d_on = min( m, duty * T - m );           // signed: > 0 inside an on-run
            cov = min( cov, saturate( 0.5 + d_on * ra ) );
        }
        return cov;
    }

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
    }

    // GUI_TEX_OP_BAND -- the band of `border` px lying inside the boundary: the rounded outline.
    // The one op that bends `d` rather than the coverage, because a band IS a different field and
    // everything downstream should measure from its edges, not the original boundary's.
    if ( ( tex & TEX_OP_BAND ) != 0u )
        d = abs( d + border * 0.5 ) - border * 0.5;

    float cov = ( feather <= 0.0 ) ? ( d <= 0.0 ? 1.0 : 0.0 )
                                   : saturate( 0.5 - d / feather );

    // GUI_TEX_OP_CUT -- the interior cut away.  The outward half of the falloff is untouched, so a
    // cut surface and a filled one are pixel-identical everywhere the fill was visible; what goes
    // is the saturated core, which a drop shadow only ever showed through whatever it sits behind.
    // Taken here rather than folded into `d` because the cut is on COVERAGE: bending the distance
    // would move the boundary the outward falloff is measured from.
    if ( ( tex & TEX_OP_CUT ) != 0u && d <= 0.0 )
        cov = 0.0;

    // GUI_TEX_OP_INSET -- the inner shadow.  The falloff is re-measured INWARD from the boundary:
    // full strength against the edge, gone `feather` px in, and nothing outside it.  Taken on
    // COVERAGE for the same reason the cut is (bending d would move the boundary both
    // terms are measured from), and the second factor is the ordinary 1 px edge band, so the outer
    // rim antialiases exactly as the filled box's does rather than stair-stepping.
    if ( ( tex & TEX_OP_INSET ) != 0u )
        cov = saturate( 1.0 + d / max( feather, 1e-4 ) ) * saturate( 0.5 - d );

    // GUI_TEX_OP_PULSE -- reuses the same two shifts for radius/feather and reads the top 7 bits
    // as rate+depth, which is why it is the one op that cannot combine with BAND (those bits are
    // `border` there).  The wave starts at its PEAK (cos 0 = 1 -> no attenuation), so a pulse
    // fading in from nothing is never what the first frame shows.
    if ( ( tex & TEX_OP_PULSE ) != 0u )
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

// The clip band: which clip rect cuts this fragment, resolved HERE rather than by the hardware
// scissor so a clip change never opens a new draw call.  The vertex names its entry (bits 19..27
// of the tex word, absolute within the frame's clip region); pc.clip_base is the region's origin,
// constant for the whole flush.  The table lives in a bindless storage buffer named by
// pc.clip_buf.  clip_buf 0 -- the reserved invalid slot -- means "no clip table bound": full
// coverage, so a pipeline user without a table (and the gui itself before its first upload) is
// never clipped by garbage.
//
// Entries are two float4s: [0] = (x0, y0, x1, y1) pixel EDGES, pre-snapped CPU-side to the same
// grid the hardware scissor was, so radius-0 coverage is bit-identical to the scissor it
// replaces (fragment centres sit at .5 against integer edges; no AA band on purpose -- a clip is
// a cut, not a shape).  radius > 0 evaluates the rounded-box field with a 1 px AA band instead:
// the clip a scissor rect could never express.
float clip_coverage( uint tex, float2 p )
{
    if ( pc.clip_buf == 0u )
        return 1.0;
    uint   e   = ( pc.clip_base + ( ( tex >> TEX_CLIP_SHIFT ) & TEX_CLIP_MASK ) ) * 2u;
    float4 r   = u_buffers[ pc.clip_buf ][ e ];
    float  rad = u_buffers[ pc.clip_buf ][ e + 1u ].x;
    if ( rad <= 0.0 )
        return ( p.x >= r.x && p.y >= r.y && p.x < r.z && p.y < r.w ) ? 1.0 : 0.0;
    float2 c = ( r.xy + r.zw ) * 0.5;
    float2 h = ( r.zw - r.xy ) * 0.5 - float2( rad, rad );
    float2 q = abs( p - c ) - h;
    float  d = min( max( q.x, q.y ), 0.0 ) + length( max( q, float2( 0.0, 0.0 ) ) ) - rad;
    return saturate( 0.5 - d );
}

float4 main( ps_in_t i ) : SV_Target0
{
    // The clip cut applies to EVERYTHING, the debug views included -- the scissor this replaces
    // cut their geometry too, and a batch-view block that ignored its clip would lie about what
    // the draw painted.
    float ccov = clip_coverage( i.tex, i.sv_pos.xy );

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
                           float( ( pc.dbg_tint >> 24 ) & 0xFFu ) / 255.0 * ccov );
        }
        return float4( i.color.rgb, ccov );   // wireframe keeps each window's own (linear) color
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
    float  cov = fx_coverage( i.fx_coord, i.fx, i.tex, i.uv, i.sv_pos.xy ) * ccov;

    // SELF-SAMPLED primitives (GUI_TEX_SELF_BIT): the shape is solid colour, so the texel is not
    // consulted -- the uv word carried mode parameters instead (the sample above read a garbage
    // location of a VALID texture, which is harmless and cheaper than a divergent skip).  The bit
    // rides the TEX word, not the mode number, so whether a uv is a payload is the emit site's
    // call: the same mode fills solid here and carries real texcoords through draw_texture_in.
    // Mode 10 additionally sweeps the colour toward a second RGBA8 riding that word, lerped by the
    // same signed angle the aperture cut uses -- the gradient a 4-corner vertex colour cannot
    // express.  Mode 11 picks between the vertex colour and that same second RGBA8 by cell parity.
    float4 vcol = i.color;
    uint   fxm  = i.fx & 0xFu;
    if ( ( i.tex & TEX_SELF_BIT ) != 0u )
    {
        s = float4( 1.0, 1.0, 1.0, 1.0 );
        if ( fxm == 10u )
        {
            uint   bu = uint( round( i.uv.x * 65535.0 ) );
            uint   bv = uint( round( i.uv.y * 65535.0 ) );
            float4 c2 = float4( float( bu & 0xFFu ), float( bu >> 8 ),
                                float( bv & 0xFFu ), float( bv >> 8 ) ) / 255.0;
            c2.rgb    = srgb_to_linear( c2.rgb );   // authored sRGB, never saw the vertex stage
            float ap  = float( ( i.fx >> 23 ) & 0x1FFu ) * ( 3.14159265 / 511.0 );
            float t   = saturate( ( atan2( i.fx_coord.x, i.fx_coord.y ) + ap )
                                  / max( 2.0 * ap, 1e-4 ) );
            vcol = lerp( vcol, c2, t );
        }
        // mode 11 CHECKER -- col_b decodes exactly as GRAD's does; the cell pitch and the parity
        // phase come from the effect word (phase is a fraction of the TWO-cell colour period, so
        // /255 * 2*cell = cell/127.5).  Parity is floor-based like every pattern mod here (fmod
        // truncates), and odd cells take the second colour.
        else if ( fxm == 11u )
        {
            uint   bu = uint( round( i.uv.x * 65535.0 ) );
            uint   bv = uint( round( i.uv.y * 65535.0 ) );
            float4 c2 = float4( float( bu & 0xFFu ), float( bu >> 8 ),
                                float( bv & 0xFFu ), float( bv >> 8 ) ) / 255.0;
            c2.rgb     = srgb_to_linear( c2.rgb );   // authored sRGB, never saw the vertex stage
            float  cell = float( ( i.fx >>  4 ) & 0xFFFu ) * 0.25;
            float2 ph   = float2( float( ( i.fx >> 16 ) & 0xFFu ),
                                  float( ( i.fx >> 24 ) & 0xFFu ) ) * ( cell / 127.5 );
            float2 t    = ( i.sv_pos.xy - ph ) / cell;
            float  k    = floor( t.x ) + floor( t.y );
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

    // vcol.rgb is already linear light (i.color, or the mode-10 sweep of it); alpha is coverage,
    // which was linear all along.  s.r is the glyph coverage from the R8 atlas (1.0 for the white
    // solid-color pixel and for the self-sampled modes, so non-text draws pass through).
    return float4( vcol.rgb, vcol.a * s.r * cov );
}
