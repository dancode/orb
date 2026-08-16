#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler   u_samplers[];

// The bindless storage-buffer array (set 0, binding 2).  The gui reads TWO tables through it:
//   - the frame's clip entries, two vec4s each (see clip_coverage below)
//   - the frame's PRIMITIVE RECORDS, five vec4s each (gui.h, gui_prim_t)
// Both are declared as vec4[] because that is the one element type the array can have; the
// record's integer row comes back through floatBitsToUint, which is a reinterpret, not a convert.
layout(set = 0, binding = 2, std430) readonly buffer clip_buf_t { vec4 data[]; } u_buffers[];

layout(push_constant) uniform PC {
    mat4 mvp;
    uint samp_point;  // bindless sampler slot: NEAREST, for the coverage model
    uint samp_image;  // bindless sampler slot: LINEAR, for every model that filters
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    float time;      // effect-band frame clock, seconds wrapped to GUI_FX_TIME_WRAP (1024)
    uint clip_buf;   // bindless buffer slot of the frame's clip table (0 = no table, no clipping)
    uint clip_base;  // the flush's clip-region origin in the table (entries, not vec4s)
    uint prim_buf;   // bindless buffer slot of the frame's primitive records
    uint prim_base;  // this window SLOT's first record (records, not vec4s)
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 2) in  vec2 v_fx_coord;
layout(location = 3) flat in uint v_prim;  // primitive record index, slot-local
layout(location = 0) out vec4 out_color;

// Mirrors gui.h: the record's op bits and the sampling model in its `tex` field.  Keep gui.h,
// gui.frag and gui.ps.hlsl in step, then resplice the SPIR-V and re-cook.
#define OP_BAND         0x01u
#define OP_CUT          0x02u
#define OP_INSET        0x04u
#define OP_PULSE        0x08u
#define OP_STRIPES      0x10u
#define OP_SELF         0x20u

#define TEX_MODE_SHIFT  28u
#define TEX_INDEX_MASK  0x0FFFFFFFu

// Five vec4 rows per record, no padding (gui.h pins the struct to that).
#define PRIM_ROWS       5u

// The record this fragment's primitive named, resolved once at the top of main().  Row 0 is what
// every fragment reads -- a glyph or a flat fill decodes its texture and clip and touches nothing
// else -- so it is held in these three, and the four rows below it are fetched only inside the
// branches that want them.  Globals rather than parameters because fx_coverage() would otherwise
// take five arguments that are all the same record.
uint  g_row;      // first vec4 of the record, absolute in the buffer
uint  g_field;    // gui_fx_mode_t
uint  g_ops;      // GUI_OP_*
uint  g_tex;      // sampling model | bindless slot

vec4 prim_row( uint r )
{
    return u_buffers[ pc.prim_buf ].data[ g_row + r ];
}

// v_color arrives ALREADY LINEAR -- the vertex stage decodes it (see gui.vert), because it is a
// per-vertex constant and decoding it per fragment spent three pow() on every pixel of the UI.
// Nothing below this line linearizes the vertex color; doing so would decode it twice.
//
// This function survives for the colors that do NOT come down the pipe: the debug batch tint and
// the record's second colour, both authored sRGB and read straight out of a 32-bit word.
vec3 srgb_to_linear( vec3 c )
{
    bvec3 cutoff = lessThanEqual( c, vec3( 0.04045 ) );
    vec3  lo     = c / 12.92;
    vec3  hi     = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
    return mix( hi, lo, vec3( cutoff ) );
}

// An RGBA8 word (R in the low byte) as linear-light colour.  The record carries its second colour
// this way -- CHECKER's alternate, ARC_GRAD's far end, TEXT_EDGE's outline -- and all three are
// authored sRGB, so they decode here rather than in the vertex stage they never passed through.
vec4 unpack_col( uint c )
{
    vec4 v = vec4( float(   c         & 0xFFu ), float( ( c >>  8 ) & 0xFFu ),
                   float( ( c >> 16 ) & 0xFFu ), float( ( c >> 24 ) & 0xFFu ) ) / 255.0;
    return vec4( srgb_to_linear( v.rgb ), v.a );
}

// The effect band (gui.h): coverage of the shape this fragment's record names, 1.0 when it names
// none.  v_fx_coord is |p| - c, so the corner arc the CPU used to tessellate is simply where both
// components go positive at once.
//   field 1 BOX -- fill inside the boundary
//   field 6 SEG -- a capsule (below)
// Those are the two shapes that reach the shared decode at the bottom, and the OP bits modify
// whichever one arrived: BAND bends the field into a border, CUT and INSET cut the coverage
// against the boundary in opposite directions, PULSE breathes it on pc.time.  Every op therefore
// applies to both shapes -- a stroked capsule needs no field of its own.
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
float fx_coverage()
{
    // 0 NONE, and the two fields that are not shapes: 4 TILE_U acts on the texcoord in main(), 5
    // TEXT_EDGE acts on the COLOR in the SDF branch below.  All three contribute full coverage --
    // the band names what the fragment does, and "nothing, here" is a legal answer.
    if ( g_field == 0u || g_field == 4u || g_field == 5u )
        return 1.0;

    // fields 11 CHECKER / 12 GRID -- the framebuffer-tiling patterns (gui.h).  Both compute in
    // gl_FragCoord pixels: exact at any panel size, where the HALF2 effect coordinate's ulp
    // reaches a full pixel at the corners of a fullscreen backdrop.  The phase re-anchors the
    // pattern to the shape; the CPU derived it against the same quantized cell the record carries.
    if ( g_field == 11u || g_field == 12u )
    {
        if ( g_field == 11u )
            return 1.0;   // CHECKER cuts nothing: it picks between two colours in main()

        // GRID: distance to the nearest lattice line, the line `thickness` px wide straddling
        // it, resolved with a 1 px AA band.  The mod is floor-based on purpose: the
        // phase-shifted coordinate can go negative near the origin.
        vec4  parm = prim_row( 4u );
        float cell = parm.x;
        float ht   = parm.y * 0.5;      // HALF the line width
        float ang  = parm.z;
        vec2  ph   = prim_row( 2u ).xy; // per-axis phase, as a fraction of the cell

        // Into LATTICE space first, so everything below is the axis-aligned case it always was.
        // The rotation is of the pixel coordinate, not of the pattern: the phase the CPU sent was
        // computed against the ROTATED anchor for exactly this reason (tess_grid), so the lines
        // still land on the anchor after the turn instead of sliding off it.
        vec2 q = gl_FragCoord.xy;
        if ( ang != 0.0 )
        {
            float cs = cos( ang ), sn = sin( ang );
            q = vec2( q.x * cs + q.y * sn, -q.x * sn + q.y * cs );
        }

        vec2 p  = q - ph * cell;
        vec2 m  = p - cell * floor( p / cell );
        vec2 dl = min( m, vec2( cell ) - m );

        // STRIPES cuts on ONE axis instead of both: a lattice becomes a stripe field, and a stripe
        // field at an angle is the diagonal hatch.  Same quad, same cost.
        float d = ( ( g_ops & OP_STRIPES ) != 0u ) ? dl.x : min( dl.x, dl.y );
        return clamp( 0.5 + ht - d, 0.0, 1.0 );
    }

    // fields 7 ARC and 8 PIE -- circular sectors, plus their two self-sampled variants.  Taken
    // before the shared decode below because a sector has no feather (it gets a fixed 1 px band,
    // which is what `0.5 - d` is) and reads an aperture nothing else has.
    //
    // v_fx_coord is SIGNED here, not folded, and already rotated by the CPU so the sector's
    // bisector points +y (gui.h).  That is the whole trick: a circular shape subtracts no
    // half-extent, so its coordinate is affine over one quad and the sign survives -- and the sign
    // is the angle, without which neither of these shapes can be expressed at all.  The fold below
    // is the fragment's own, exact because the value it folds is exact.
    if ( g_field >= 7u && g_field <= 10u )
    {
        vec4  parm = prim_row( 4u );
        float ra   = parm.x;
        float rb   = parm.y;
        float ap   = parm.z;
        vec2  sc   = vec2( sin( ap ), cos( ap ) );   // the aperture as a direction, once per fragment
        vec2  q    = vec2( abs( v_fx_coord.x ), v_fx_coord.y );
        float ds;

        if ( g_field == 8u )
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
            // Fields 9 (ARC_DASH) and 10 (ARC_GRAD) are this same band -- the dash cut is below,
            // the gradient acts on the COLOR in main() (the TEXT_EDGE precedent).
            ds = ( ( sc.y * q.x > sc.x * q.y ) ? length( q - sc * ra )
                                               : abs( length( q ) - ra ) ) - rb;
        }
        float cov = clamp( 0.5 - ds, 0.0, 1.0 );

        // field 9 ARC_DASH -- the band cut by an angular dash pattern.  theta is the SIGNED angle
        // from the bisector (the same signed coordinate the aperture cut uses); the pattern anchors
        // at the sweep start, and the emit side pre-quantized the period so whole cycles fit -- a
        // closed ring has no seam.  The cut edge is antialiased in ARC-LENGTH pixels (angle error
        // times ra).  The mod is floor-based: the cap's small negatives need floor, not trunc.
        if ( g_field == 9u )
        {
            vec2  dash = prim_row( 2u ).xy;                    // period (turns), on-duty fraction
            float T    = max( dash.x, 1.0 / 65535.0 ) * 6.28318531;
            float duty = dash.y;
            float t    = atan( v_fx_coord.x, v_fx_coord.y ) + ap;
            float m    = t - T * floor( t / T );
            float d_on = min( m, duty * T - m );          // signed: > 0 inside an on-run
            cov = min( cov, clamp( 0.5 + d_on * ra, 0.0, 1.0 ) );
        }
        return cov;
    }

    vec4  soft    = prim_row( 3u );
    float feather = soft.x;
    float border  = soft.y;

    // The corner radius, picked PER QUADRANT.  The field itself still arrives folded in
    // v_fx_coord, but which of the four radii that fold used is a property of where the fragment
    // sits, so it comes from the record and the rasterizer's own pixel coordinate rather than
    // from a per-quadrant copy of a packed word.  This is the first piece of the fold to move.
    vec4  rect   = prim_row( 1u );
    vec2  dp     = gl_FragCoord.xy - rect.xy;
    vec2  local  = vec2( dp.x * soft.z + dp.y * soft.w,     // un-rotate: R(-rot) * (frag - centre)
                        -dp.x * soft.w + dp.y * soft.z );
    vec4  rad    = prim_row( 2u );
    float radius = ( local.y <= 0.0 ) ? ( ( local.x <= 0.0 ) ? rad.x : rad.y )
                                      : ( ( local.x <= 0.0 ) ? rad.w : rad.z );

    vec2  q = v_fx_coord;
    float d;

    // field 6 SEG -- a CAPSULE: the distance to a line segment, minus its half-thickness.  q.x is
    // |along| - halflen and q.y is the SIGNED across-axis offset, which needs no fold because
    // length() squares it.  That asymmetry is the point: the box folds both axes at the vertex
    // because both subtract a half-extent, so it costs four quadrant quads; the segment subtracts
    // on one axis only and costs two.  No interior term either -- this form is already the exact
    // signed distance in the core, where the rounded box's length-only form saturates.
    if ( g_field == 6u )
    {
        radius = rad.x;                                  // a capsule has one radius, not four
        d = length( vec2( max( q.x, 0.0 ), q.y ) ) - radius;
    }
    else
    {
        d = min( max( q.x, q.y ), 0.0 ) + length( max( q, vec2( 0.0 ) ) ) - radius;
    }

    // GUI_OP_BAND -- the band of `border` px lying inside the boundary: the rounded outline.
    // The one op that bends `d` rather than the coverage, because a band IS a different field and
    // everything downstream should measure from its edges, not the original boundary's.
    if ( ( g_ops & OP_BAND ) != 0u )
        d = abs( d + border * 0.5 ) - border * 0.5;

    float cov = ( feather <= 0.0 ) ? ( d <= 0.0 ? 1.0 : 0.0 )
                                   : clamp( 0.5 - d / feather, 0.0, 1.0 );

    // GUI_OP_CUT -- the interior cut away.  The outward half of the falloff is untouched, so a
    // cut surface and a filled one are pixel-identical everywhere the fill was visible; what goes
    // is the saturated core, which a drop shadow only ever showed through whatever it sits behind.
    // Taken here rather than folded into `d` because the cut is on COVERAGE: bending the distance
    // would move the boundary the outward falloff is measured from.
    if ( ( g_ops & OP_CUT ) != 0u && d <= 0.0 )
        cov = 0.0;

    // GUI_OP_INSET -- the inner shadow.  The falloff is re-measured INWARD from the boundary:
    // full strength against the edge, gone `feather` px in, and nothing outside it.  Taken on
    // COVERAGE for the same reason the cut is (bending d would move the boundary both
    // terms are measured from), and the second factor is the ordinary 1 px edge band, so the outer
    // rim antialiases exactly as the filled box's does rather than stair-stepping.
    if ( ( g_ops & OP_INSET ) != 0u )
        cov = clamp( 1.0 + d / max( feather, 1e-4 ), 0.0, 1.0 ) * clamp( 0.5 - d, 0.0, 1.0 );

    // GUI_OP_PULSE -- breathe the coverage on the frame clock.  It composes with BAND now: rate
    // and depth are their own fields in the record, where the packed word had to spend `border`'s
    // bits on them and the two ops could never be set together.
    // The wave starts at its PEAK (cos 0 = 1 -> no attenuation), so a pulse fading in from nothing
    // is never what the first frame shows.
    if ( ( g_ops & OP_PULSE ) != 0u )
    {
        vec4  parm  = prim_row( 4u );
        float rate  = parm.x;
        float depth = parm.y;
        cov *= 1.0 - depth * ( 0.5 - 0.5 * cos( 6.28318531 * rate * pc.time ) );
    }

    return cov;
}

// The clip band: which clip rect cuts this fragment, resolved HERE rather than by the hardware
// scissor so a clip change never opens a new draw call.  The record names its entry, absolute
// within the frame's clip region; pc.clip_base is the region's origin, constant for the whole
// flush.  The table lives in a bindless storage buffer named by pc.clip_buf.  clip_buf 0 -- the
// reserved invalid slot -- means "no clip table bound": full coverage, so a pipeline user without
// a table (and the gui itself before its first upload) is never clipped by garbage.
//
// Entries are two vec4s: [0] = (x0, y0, x1, y1) pixel EDGES, pre-snapped CPU-side to the same
// grid the hardware scissor was, so radius-0 coverage is bit-identical to the scissor it
// replaces (fragment centres sit at .5 against integer edges; no AA band on purpose -- a clip is
// a cut, not a shape).  radius > 0 evaluates the rounded-box field with a 1 px AA band instead:
// the clip a scissor rect could never express.
float clip_coverage( uint clip )
{
    if ( pc.clip_buf == 0u )
        return 1.0;
    uint  e   = ( pc.clip_base + clip ) * 2u;
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
    // The record, and with it everything that used to be bit-packed into the vertex.  One 16-byte
    // load: a glyph, a flat fill and a debug-view quad all resolve here and never touch the four
    // rows behind it.
    g_row      = ( pc.prim_base + v_prim ) * PRIM_ROWS;
    vec4 head  = u_buffers[ pc.prim_buf ].data[ g_row ];
    g_field    = floatBitsToUint( head.x );
    g_ops      = floatBitsToUint( head.y );
    g_tex      = floatBitsToUint( head.z );

    // The clip cut applies to EVERYTHING, the debug views included -- the scissor this replaces
    // cut their geometry too, and a batch-view block that ignored its clip would lie about what
    // the draw painted.
    float ccov = clip_coverage( floatBitsToUint( head.w ) );

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

    /* The texture and its sampling model come off the RECORD (gui.h) rather than a draw call, so
       one draw can mix a coverage atlas, an SDF atlas and arbitrary RGBA images.  nonuniformEXT is
       mandatory here and not a formality: neighbouring primitives in a single draw legitimately
       name different descriptors, so the index is not wave-uniform and indexing without it is
       undefined.
       The SAMPLER is derived from the model rather than carried -- coverage must stay point-sampled
       (a filtered glyph atlas stops being crisp) and every other model filters.  Testing "not
       COVERAGE" means a model added later lands on the filtering side without touching this. */
    uint tex_mode = g_tex >> TEX_MODE_SHIFT;
    uint tex_slot = g_tex & TEX_INDEX_MASK;
    uint samp     = ( tex_mode == 0u ) ? pc.samp_point : pc.samp_image;

    /* GUI_FX_TILE_U: the stored U is normalized 0..1 (all UNORM16X2 can hold) and the repeat count
       rides the record.  Scaling HERE rather than in the vertex stage is exactly equivalent -- the
       multiply is affine and commutes with interpolation -- and it keeps the vertex shader a pure
       pass-through, so a dashed line is still one quad tiling the atlas stipple row under the
       sampler's REPEAT. */
    vec2 uv = v_uv;
    if ( g_field == 4u )
        uv.x *= prim_row( 4u ).x;

    vec4  s   = texture( sampler2D( u_textures[nonuniformEXT( tex_slot )],
                                    u_samplers[nonuniformEXT( samp )] ), uv );
    float cov = fx_coverage() * ccov;   // every non-debug path below multiplies by cov

    // SELF-SAMPLED primitives (GUI_OP_SELF): the shape is solid colour, so the texel is not
    // consulted (the sample above read a garbage location of a VALID texture, which is harmless and
    // cheaper than a divergent skip).  The flag is an OP, not a property of the field, so whether a
    // primitive is solid is the emit site's call: the same field fills solid here and carries real
    // texcoords through draw_texture_in.
    // Field 10 additionally sweeps the colour toward the record's second colour, lerped by the same
    // signed angle the aperture cut uses -- the gradient a 4-corner vertex colour cannot express.
    // Field 11 picks between the vertex colour and that same second colour by cell parity.
    vec4 vcol = v_color;
    if ( ( g_ops & OP_SELF ) != 0u )
    {
        s = vec4( 1.0 );
        if ( g_field == 10u )
        {
            vec4  parm = prim_row( 4u );
            vec4  c2   = unpack_col( floatBitsToUint( parm.w ) );
            float ap   = parm.z;
            float t    = clamp( ( atan( v_fx_coord.x, v_fx_coord.y ) + ap )
                                / max( 2.0 * ap, 1e-4 ), 0.0, 1.0 );
            vcol = mix( vcol, c2, t );
        }
        // field 11 CHECKER -- the cell pitch and the parity phase come from the record, the phase
        // as a fraction of the TWO-cell colour period (one cell of phase would swap the colours).
        // Parity is floor-based like every pattern mod here, and odd cells take the second colour.
        else if ( g_field == 11u )
        {
            vec4  parm = prim_row( 4u );
            vec4  c2   = unpack_col( floatBitsToUint( parm.w ) );
            float cell = parm.x;
            vec2  ph   = parm.yz * ( 2.0 * cell );
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
        if ( g_field == 5u )
        {
            vec4  parm  = prim_row( 4u );
            float wpx   = parm.x;
            vec4  ecol  = unpack_col( floatBitsToUint( parm.w ) );

            // Source-over of the fill onto the band, resolved analytically: the band contributes
            // only where the fill does not (1 - fill), so the seam between them is antialiased by
            // the same coverage that antialiases the glyph, and the two never double-darken.
            float outer = clamp( dpx + wpx + 0.5, 0.0, 1.0 );
            float af    = v_color.a * fill;
            float ao    = ecol.a * outer * ( 1.0 - fill );
            float at    = af + ao;
            out_color   = vec4( ( v_color.rgb * af + ecol.rgb * ao ) / max( at, 1e-6 ), at * cov );
            return;
        }

        out_color = vec4( v_color.rgb, v_color.a * cov * fill );
        return;
    }

    // vcol.rgb is already linear light (v_color, or the field-10 sweep of it); alpha is coverage,
    // which was linear all along.  s.r is the glyph coverage from the R8 atlas (1.0 for the white
    // solid-color pixel and for the self-sampled fields, so non-text draws pass through).
    out_color = vec4( vcol.rgb, vcol.a * s.r * cov );
}
