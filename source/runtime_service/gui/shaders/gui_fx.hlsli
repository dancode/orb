// gui_fx.hlsli -- the gui fragment stage: the whole effect band.  Included by gui_quad.ps.hlsl
// (the bufferless pipeline: placement and clip arrive as interpolants from the pull vertex
// stage, and the record is a pure STYLE).
//
// What this stage resolves, and where the model it implements is written down:
//   - the STYLE RECORD (gui.h, gui_prim_t) -- shape, modifiers, texture, and every shape
//     parameter, reached through the one slot-local index the quad carries.  gui.h is the
//     layout and the field / op catalogue; the code below is the evaluation.
//   - the CLIP TABLE -- resolved here rather than by the hardware scissor, so a clip change
//     never opens a draw call and a clip can have a corner radius.
//   - the SAMPLING MODEL -- the top 4 bits of the record's `tex`, which chooses both what a
//     texel means and which sampler reads it.

#include "gui_common.hlsli"

[[vk::binding( 0, 0 )]] Texture2D    u_textures[] : register( t0, space0 );
[[vk::binding( 1, 0 )]] SamplerState u_samplers[] : register( s0, space0 );

// Mirrors gui.h: the record's op bits and the sampling model in its `tex` field.  These two
// declarations of the same constants are the only duplication the record model still carries;
// keep them in step and rebuild.
#define OP_BAND         0x01u
#define OP_CUT          0x02u
#define OP_INSET        0x04u
#define OP_PULSE        0x08u
#define OP_STRIPES      0x10u
#define OP_SELF         0x20u
#define OP_GRAD         0x40u
#define OP_GRAD_RADIAL  0x80u
#define OP_GRAD_CONIC   0x100u
#define OP_SPIN         0x200u
#define OP_DASH         0x400u
#define OP_DITHER       0x800u
#define OP_FRAME        0x1000u

// The PATTERN ops: what a shape is filled or cut WITH, rather than what shape it is.  All read
// row 7 and at most one is live per record.
#define OP_TILE_U       0x2000u
#define OP_TEXT_EDGE    0x4000u
#define OP_CHECKER      0x8000u
#define OP_GRID         0x10000u

// The ops that read the animation clock.  One test decides whether row 5 is fetched at all, so a
// static shape never pays for the timebase it does not use.
#define OP_ANIMATED     ( OP_PULSE | OP_SPIN | OP_DASH )

// gui_curve_t: the shaping stage between the timebase and the effect.
#define CURVE_LINEAR    0u
#define CURVE_SINE      1u
#define CURVE_TRIANGLE  2u
#define CURVE_SMOOTH    3u
#define CURVE_EASE      4u
#define CURVE_STAIR     5u
#define CURVE_SQUARE    6u
#define CURVE_DECAY     7u

#define TEX_MODE_SHIFT  28u
#define TEX_INDEX_MASK  0x0FFFFFFFu

#define PI              3.14159265

struct ps_in_t
{
    float4                 sv_pos : SV_Position;
    float4                 color  : COLOR0;
    float2                 uv     : TEXCOORD0;
    nointerpolation uint   prim   : TEXCOORD1;   // style record index, slot-local
    nointerpolation float4 rect   : TEXCOORD2;   // shape placement: centre + half-extent (per quad)
    nointerpolation uint   clip   : TEXCOORD3;   // clip-table entry index (per quad)
    nointerpolation float4 col2   : TEXCOORD4;   // GUI_OP_FRAME: the border band's colour --
                                                  //   rides the quad, never the style
    nointerpolation float4 inst   : TEXCOORD5;   // per-instance lanes off the quad: xy = the turn
                                                  //   (cos, sin), z = animation phase in cycles
};

// The record this fragment's primitive named, resolved once at the top of main().  Row 0 is what
// every fragment reads -- a glyph or a flat fill decodes its texture and clip and touches nothing
// else -- so it is held in these, and the rows below it are fetched only inside the branches
// that want them.  Globals rather than parameters because fx_coverage() would otherwise take five
// arguments that are all the same record.
static uint g_row;      // first float4 of the record, absolute in the buffer
static uint g_field;    // gui_fx_mode_t
static uint g_ops;      // GUI_OP_*
static uint g_tex;      // sampling model | bindless slot

// The per-instance lanes, off the quad rather than the style: the shape's turn, and the animation
// phase in cycles.  Every animating op reads the phase the same way, which is what lets one style
// drive a whole staggered set.
static float2 g_rot   = float2( 1.0, 0.0 );
static float  g_phase = 0.0;

// The animation clock, resolved once in main() and read by every animating op instead of pc.time.
// g_phi is where in the cycle this fragment sits; g_k is that phase after the record's curve has
// shaped it.  Splitting them is what makes a stepped spinner, an eased dash and a square-wave
// blink the same mechanism -- and what keeps two ops on one record moving together, since they
// share the phase rather than each deriving their own.
static float g_phi = 0.0;
static float g_k   = 0.0;

// OP_SPIN's rotation on the frame clock, resolved once per fragment in main() and composed into
// every shape-local frame prim_local derives.  Identity when the op is absent, so the compose
// below costs the same four multiplies either way.
static float g_spin_c = 1.0;
static float g_spin_s = 0.0;

// OP_FRAME's border-band coverage, stashed by fx_coverage for main() to composite over the fill
// -- the band and the fill resolve from the same field evaluation, so they cannot disagree.
static float g_frame_band = 0.0;

float4 prim_row( uint r )
{
    return u_buffers[ pc.prim_buf ][ g_row + r ];
}

// The SHAPING stage: a normalized phase in, a normalized amount out.  It knows nothing about what
// it drives, which is the point -- the same eight curves bend a rotation, a dash offset and a
// coverage breath, so easing is one mechanism rather than one per effect.
//
// Every curve leaves 0 at phase 0 except DECAY, which starts at its peak and falls: a flash is
// exactly the thing that has already happened by the time you see it start.
float fx_curve( float phi, uint kind, float p )
{
    switch ( kind )
    {
        case CURVE_SINE:     return 0.5 - 0.5 * cos( 6.28318531 * phi );
        case CURVE_TRIANGLE: return 1.0 - abs( 2.0 * phi - 1.0 );
        case CURVE_SMOOTH:   return phi * phi * ( 3.0 - 2.0 * phi );
        case CURVE_EASE:     return pow( phi, max( p, 1e-3 ) );
        // Steps of equal height, the last one landing exactly on 1 -- a hand that ticks between
        // positions.  Fewer than two steps is not a staircase, so it clamps rather than dividing
        // by zero.
        case CURVE_STAIR:  { float n = max( p, 2.0 ); return floor( phi * n ) / ( n - 1.0 ); }
        case CURVE_SQUARE:   return ( phi < p ) ? 0.0 : 1.0;
        case CURVE_DECAY:    return exp( -max( p, 0.0 ) * phi );
        default:             return phi;                                  // CURVE_LINEAR
    }
}

// The shape's PLACEMENT -- centre and half-extent, off the per-quad interpolant and cached in a
// global by main(), which is what lets one style serve every placement.
static float4 g_rect;
float4 prim_rect( void ) { return g_rect; }

// The shape-local coordinate: the pixel position, moved to the quad's centre and un-rotated by the
// quad's own turn.  Every field that has a shape works in this frame, and every one of them used
// to receive it interpolated from a per-vertex HALF2 -- which could only be correct within one
// quadrant, because the fold that produced it is not affine.  Derived here it is exact everywhere,
// which is what collapsed the box from four quads to one.
//
// SV_Position is the pixel CENTRE (i + 0.5) and the quad's centre is in the same pixel space, the
// same convention clip_coverage already relies on.
float2 prim_local( float2 px, float4 rect )
{
    // OP_SPIN adds its clock-driven angle to the quad's turn by composing the two (cos, sin) pairs
    // -- the angle-sum identity, not a second rotate.  g_spin is identity when the op is absent.
    // The frame carries every field and op with it, which is what makes a spinner, a radar sweep
    // and a rotating dashed ring one op over shapes that already exist.
    float cz = g_rot.x * g_spin_c - g_rot.y * g_spin_s;
    float sw = g_rot.y * g_spin_c + g_rot.x * g_spin_s;
    float2 d = px - rect.xy;
    return float2( d.x * cz + d.y * sw, -d.x * sw + d.y * cz );
}

// The rounded-box field at `p`, in the shape's own local frame (prim_local's frame).  The corner
// radius comes from the SIGN of p -- which quadrant the fragment sits in -- so four different
// corners share one record and one quad.
//
// The min(max(q.x,q.y),0) term is the INTERIOR distance and it is not optional: without it the
// field saturates at -radius everywhere inside, which silently breaks the two cases that need
// depth rather than proximity -- a border wider than the radius (the whole interior lands in the
// band and fills), and a shadow whose falloff is wider than the radius (its core never reaches
// full opacity).
//
// `pw` is the corner's PROFILE: the exponent of the norm the outside term is measured in.  At 2
// that norm is length() and the corner is a circular arc; above it the arc fills out toward the
// square it is inset from, which is the continuous "squircle" corner -- curvature that ramps in
// over the whole corner instead of starting abruptly where the arc meets the edge.  0 means the
// default, and the compare below sends it down the exact same instruction as 2.
//
// The Lp form is not a Euclidean distance, so the field's gradient is shorter along the corner
// diagonal than on the flats -- about 0.84 at pw 4 -- and the antialiasing band there is wider by
// the reciprocal.  At the exponents a UI wants (2..6) that is a fraction of a pixel on the one
// part of the outline that is already curved.
float box_field( float2 p, float2 half_ext, float4 rad, float pw )
{
    float  r = ( p.y <= 0.0 ) ? ( ( p.x <= 0.0 ) ? rad.x : rad.y )
                              : ( ( p.x <= 0.0 ) ? rad.w : rad.z );
    float2 q = abs( p ) - ( half_ext - float2( r, r ) );
    float2 m = max( q, float2( 0.0, 0.0 ) );

    float outside;
    if ( pw > 2.0 )
        outside = pow( pow( m.x, pw ) + pow( m.y, pw ), 1.0 / pw );
    else
        outside = length( m );

    return min( max( q.x, q.y ), 0.0 ) + outside - r;
}

// The PERIMETER COORDINATE of the rounded box: arc-length px from the top edge's left end to the
// boundary point nearest `p`, walked clockwise -- four edges and four corner arcs, pieced
// together from the same quadrant fold box_field runs.  OP_DASH cuts coverage on this axis, which
// is what makes a dashed border meet itself: the emit site snapped the period so whole cycles fit
// the total length this function reaches at the top-left arc's far end.
float box_perimeter_s( float2 p, float2 he, float4 rad )
{
    float HP   = 1.57079633;
    float lt   = 2.0 * he.x - rad.x - rad.y;    // top edge length; the arcs are r * pi/2
    float lr   = 2.0 * he.y - rad.y - rad.z;
    float lb   = 2.0 * he.x - rad.z - rad.w;
    float s_tr = lt;                             // cumulative starts, clockwise
    float s_r  = s_tr + rad.y * HP;
    float s_br = s_r  + lr;
    float s_b  = s_br + rad.z * HP;
    float s_bl = s_b  + lb;
    float s_l  = s_bl + rad.w * HP;
    float s_tl = s_l  + 2.0 * he.y - rad.w - rad.x;

    // The quadrant fold: q positive on both axes means the corner arc is nearest; otherwise the
    // larger component names the nearer edge.  Same radius-by-sign pick as box_field.
    float  r = ( p.y <= 0.0 ) ? ( ( p.x <= 0.0 ) ? rad.x : rad.y )
                              : ( ( p.x <= 0.0 ) ? rad.w : rad.z );
    float2 q = abs( p ) - ( he - float2( r, r ) );

    if ( q.x > 0.0 && q.y > 0.0 )
    {
        // The angle within the arc, 0 where the arc leaves its leading edge.  Unfolding the
        // quadrant mirror flips the axis order on the two corners the clockwise walk enters
        // vertically -- which works out to swapping atan2's arguments.
        float t  = ( ( p.x > 0.0 ) == ( p.y > 0.0 ) ) ? atan2( q.y, q.x ) : atan2( q.x, q.y );
        float sc = ( p.y <= 0.0 ) ? ( ( p.x > 0.0 ) ? s_tr : s_tl )
                                  : ( ( p.x > 0.0 ) ? s_br : s_bl );
        return sc + r * t;
    }
    if ( q.x > q.y )    // a vertical edge: right runs down, left runs up
        return ( p.x > 0.0 ) ? s_r + clamp( p.y + he.y - rad.y, 0.0, lr )
                             : s_l + clamp( he.y - rad.w - p.y, 0.0, s_tl - s_l );
    // a horizontal edge: top runs right, bottom runs left
    return ( p.y <= 0.0 ) ? clamp( p.x + he.x - rad.x, 0.0, lt )
                          : s_b + clamp( he.x - rad.z - p.x, 0.0, lb );
}

/*==================================================================================================
    THE EFFECT BAND, AS A CASCADE.

    Every field resolves to the same four values and then STOPS.  The ops that follow read only
    those, so no op has to know which shape it landed on and no shape reimplements an op -- that
    symmetry is the whole point, and its absence is what used to force separate fields for
    "an arc, but dashed".

        d       signed distance to the boundary, px, negative inside
        s       the coordinate ALONG the boundary, px -- the axis DASH cuts on
        aa      the width d resolves through (the style's feather, or a field's own default)
        mul     coverage from a field that has no boundary at all -- the lattice multiplies here
        shaped  false = no boundary: the d-based ops sit this one out, the rest still apply

    pc.time -- the wrapped frame clock -- is the animation seam, and no op reads it directly: it
    becomes g_phi (where in the cycle, staggered per quad by g_phase) and then g_k (that phase
    shaped by the record's curve).  Frame-constant: no re-emit, no batch split.
==================================================================================================*/

struct fx_field_t
{
    float d;
    float s;
    float aa;
    float mul;
    bool  shaped;
};

// d -> coverage.  aa 0 is a hard edge on purpose: a crisp square frame asks for exactly that.
float fx_resolve( float d, float aa )
{
    return ( aa <= 0.0 ) ? ( d <= 0.0 ? 1.0 : 0.0 ) : saturate( 0.5 - d / aa );
}

// The band of `w` px lying inside the boundary -- the shared construction under BAND and FRAME.
float fx_to_band( float d, float w )
{
    return abs( d + w * 0.5 ) - w * 0.5;
}

// A dash pattern cut on the boundary coordinate, antialiased in the px the pattern is stated in.
// The mod is floor-based on purpose: the phase can go negative and fmod truncates.
float fx_dash_cut( float s, float period, float duty )
{
    float T    = max( period, 1e-3 );
    float m    = s - T * floor( s / T );
    float d_on = min( m, duty * T - m );          // signed px: > 0 inside an on-run
    return saturate( 0.5 + d_on );
}

// OP_GRID -- the line lattice, as coverage.  It works in SV_Position pixels, not the shape's local
// frame: a backdrop's pattern belongs to the screen grid, and a shape-local coordinate's ulp
// reaches a full pixel at the corners of a fullscreen panel.  pat_phase re-anchors it to the shape
// so a backdrop drags with its window instead of sliding under it.
//
// Being an op rather than a field is the whole difference between "a rectangle of graph paper" and
// "graph paper inside whatever shape this is" -- the coverage below multiplies into the field's,
// so the lattice ends at a rounded panel's boundary, a sector's caps, a capsule's ends.
float fx_grid_mul( float2 px )
{
    float4 pat  = prim_row( 7u );
    float  cell = pat.x;
    float  ht   = pat.y * 0.5;                      // HALF the line width
    float  ang  = prim_row( 2u ).w;
    float2 ph   = unpack_unorm16x2( asuint( pat.z ) );

    // Into LATTICE space first, so everything below is the axis-aligned case it always was.  The
    // rotation is of the pixel coordinate, not of the pattern: the phase the emit site sent was
    // computed against the ROTATED anchor for exactly this reason (tess_grid), so the lines still
    // land on the anchor after the turn instead of sliding off it.
    float2 q = px;
    if ( ang != 0.0 )
    {
        float cs = cos( ang ), sn = sin( ang );
        q = float2( px.x * cs + px.y * sn, -px.x * sn + px.y * cs );
    }

    float2 p  = q - ph * cell;
    float2 m  = p - cell * floor( p / cell );
    float2 dl = min( m, float2( cell, cell ) - m );

    // STRIPES cuts on ONE axis instead of both: a lattice becomes a stripe field, and a stripe
    // field at an angle is the diagonal hatch.  Same quad, same cost.
    float dd = ( ( g_ops & OP_STRIPES ) != 0u ) ? dl.x : min( dl.x, dl.y );
    return saturate( 0.5 + ht - dd );
}

/*--------------------------------------------------------------------------------------------------
    STAGE 1 -- THE FIELD.  Every shape writes the vocabulary and returns; nothing paints here.
--------------------------------------------------------------------------------------------------*/

fx_field_t fx_field( float2 px )
{
    fx_field_t f;
    f.d      = 0.0;
    f.s      = 0.0;
    f.aa     = 0.0;
    f.mul    = 1.0;
    f.shaped = false;

    // 0 NONE -- a plain fill or a glyph.  No boundary of its own, and it still reaches every
    // coverage op below, which is why a flat rect can pulse or wear a lattice.
    if ( g_field == 0u )
        return f;

    float4 rect = prim_rect();
    float4 rad  = prim_row( 1u );
    float4 soft = prim_row( 2u );

    // fields 7 ARC / 8 PIE, and 10 ARC_GRAD which is an ARC that sweeps its colour.  A sector has
    // no feather of its own, so it states the 1 px band it always drew with -- a caller that wants
    // a soft sector now simply sets one.
    //
    // The coordinate is SIGNED here, folded only on x.  That is the whole trick: the sign is the
    // ANGLE, without which neither of these shapes can be expressed at all.
    if ( g_field >= 7u && g_field <= 10u )
    {
        float4 parm = prim_row( 3u );
        float  ra   = parm.x;
        float  rb   = parm.y;
        float  ap   = parm.z;
        float2 sc   = float2( sin( ap ), cos( ap ) );  // the aperture as a direction, once per fragment
        // The sector's frame is a REFLECTION about its bisector rather than a rotation, which
        // works out to prim_local's expression with the components swapped.  det -1 is harmless:
        // the shape is symmetric about the bisector and the pipeline does not cull.
        float2 sl   = prim_local( px, rect ).yx;
        float2 q    = float2( abs( sl.x ), sl.y );

        if ( g_field == 8u )
        {
            // PIE: the disc intersected with the angular wedge.  `l` is the disc, `m` the distance
            // to the radial edge (a segment from the centre out to the rim, hence the clamp to ra),
            // and the cross product's sign says which side of that edge we are on -- so max() keeps
            // the edges SHARP where the arc's round caps would have rounded them.
            float l = length( q ) - ra;
            float m = length( q - sc * clamp( dot( q, sc ), 0.0, ra ) );
            f.d = max( l, m * sign( sc.y * q.x - sc.x * q.y ) );
        }
        else
        {
            // ARC: exact distance to the circle of radius ra, cut to the aperture, then thickened
            // by the tube.  Inside the wedge it is the annulus; outside it is the distance to the
            // nearest endpoint, which is what gives the stroke its round caps for free.
            f.d = ( ( sc.y * q.x > sc.x * q.y ) ? length( q - sc * ra )
                                                : abs( length( q ) - ra ) ) - rb;
        }

        // ARC-LENGTH from the sweep's start, which is what makes GUI_OP_DASH mean the same thing
        // on a sector as on a box: theta is the SIGNED angle from the bisector, so theta + ap is
        // the angle travelled, and radius times that is the distance along the stroke.
        f.s      = ( atan2( sl.x, sl.y ) + ap ) * ra;
        f.aa     = max( soft.x, 1.0 );
        f.shaped = true;
        return f;
    }

    float2 local = prim_local( px, rect );

    // field 6 SEG -- a CAPSULE: the distance to a line segment, minus its half-thickness.  The
    // quad's turn IS the segment's axis, so prim_local already hands back (along, across) about the
    // midpoint -- fold the along axis against the half-length and leave the across axis signed,
    // because length() squares it anyway.  No interior term: this form is already the exact signed
    // distance in the core, where the rounded box's length-only form saturates.
    if ( g_field == 6u )
    {
        float2 q = float2( abs( local.x ) - rect.z, local.y );
        f.d = length( float2( max( q.x, 0.0 ), q.y ) ) - rad.x;   // a capsule has one radius
        f.s = local.x + rect.z;                                   // distance from the start cap
    }
    // field 2 NGON -- the regular polygon: rad.y flat sides inscribed in circumradius hw, corners
    // rounded by rad.x px.  The fold reduces the plane to one edge's sector; the distance is then
    // to that edge SEGMENT, so corners are exact rather than the apothem approximation.  Rounding
    // shrinks the polygon and inflates the field back out, so the stated radius is the size drawn.
    else if ( g_field == 2u )
    {
        float  rr  = rad.x;
        float  R   = max( rect.z - rr, 1.0 );
        float  an  = PI / max( rad.y, 3.0 );
        float2 acs = float2( cos( an ), sin( an ) );
        float  a0  = atan2( local.x, -local.y );          // 0 at the TOP vertex, y-down screen
        float  T   = 2.0 * an;
        float  bn  = ( a0 - T * floor( a0 / T ) ) - an;   // floor-mod: fmod truncates negatives
        float2 q   = length( local ) * float2( cos( bn ), abs( sin( bn ) ) );
        q -= R * acs;
        q.y += clamp( -q.y, 0.0, R * acs.y );
        f.d = length( q ) * sign( q.x ) - rr;
        f.s = a0 * rect.z;    // circumcircle arc-length: the polygon's own perimeter to within the
                              // apothem ratio, which a dash pattern cannot see
    }
    // field 3 TRI -- a solid triangle: three points about the shape centre, in the style's
    // radius + param lanes (a = r_tl,r_tr  b = r_br,r_bl  c = param_a,param_b).  Exact signed
    // distance, so the edges antialias through the shared feather and BAND strokes it like any
    // other field.  The points ride the STYLE rather than the quad: a triangle is a rare shape
    // (arrowheads, markers), so a record per triangle costs less than widening every quad.
    // States no boundary coordinate -- a dashed triangle is not a shape anything asks for.
    else if ( g_field == 3u )
    {
        float4 parm = prim_row( 3u );
        float2 a  = rad.xy;
        float2 b  = rad.zw;
        float2 c  = parm.xy;
        float2 e0 = b - a, e1 = c - b, e2 = a - c;
        float2 v0 = local - a, v1 = local - b, v2 = local - c;
        float2 pq0 = v0 - e0 * clamp( dot( v0, e0 ) / dot( e0, e0 ), 0.0, 1.0 );
        float2 pq1 = v1 - e1 * clamp( dot( v1, e1 ) / dot( e1, e1 ), 0.0, 1.0 );
        float2 pq2 = v2 - e2 * clamp( dot( v2, e2 ) / dot( e2, e2 ), 0.0, 1.0 );
        float  sg = sign( e0.x * e2.y - e0.y * e2.x );
        float2 dm = min( min( float2( dot( pq0, pq0 ), sg * ( v0.x * e0.y - v0.y * e0.x ) ),
                              float2( dot( pq1, pq1 ), sg * ( v1.x * e1.y - v1.y * e1.x ) ) ),
                              float2( dot( pq2, pq2 ), sg * ( v2.x * e2.y - v2.y * e2.x ) ) );
        f.d = -sqrt( dm.x ) * sign( dm.y );
    }
    else
    {
        f.d = box_field( local, rect.zw, rad, soft.z );
        f.s = box_perimeter_s( local, rect.zw, rad );
    }

    f.aa     = soft.x;
    f.shaped = true;
    return f;
}

/*--------------------------------------------------------------------------------------------------
    STAGES 2-4 -- the ops, in the order they have to run: bend the field, resolve it, cut it.
--------------------------------------------------------------------------------------------------*/

float fx_coverage( float2 px )
{
    fx_field_t f      = fx_field( px );
    float      border = prim_row( 2u ).y;

    // STAGE 2 -- the ops that bend the FIELD.  Both need a boundary to measure from.
    //
    // FRAME's band is taken from the field BEFORE BAND touches it, so the band's outer edge lands
    // exactly on the boundary the fill's own coverage resolves against.  BAND then bends `d`
    // itself -- the one op that does -- because a band IS a different field and everything
    // downstream should measure from its edges, not the original boundary's.
    if ( f.shaped && ( g_ops & OP_FRAME ) != 0u )
        g_frame_band = fx_resolve( fx_to_band( f.d, border ), f.aa );

    if ( f.shaped && ( g_ops & OP_BAND ) != 0u )
        f.d = fx_to_band( f.d, border );

    // STAGE 3 -- field to coverage.  A field with no boundary contributes only its multiplier.
    float cov = f.shaped ? fx_resolve( f.d, f.aa ) : 1.0;
    cov *= f.mul;

    // STAGE 4 -- the ops that cut COVERAGE.
    //
    // DASH: cut along the boundary coordinate the field stated -- perimeter px on a box, arc-length
    // on a sector, distance along the axis on a segment.  anim_rate scrolls it (the marching ants)
    // and the phase, in cycles off the quad, staggers one style across many elements.
    if ( f.shaped && ( g_ops & OP_DASH ) != 0u )
    {
        float4 dsh = prim_row( 6u );
        cov = min( cov, fx_dash_cut( f.s - g_k * dsh.x * dsh.z, dsh.x, dsh.y ) );
    }

    // CUT -- the interior cut away.  The outward half of the falloff is untouched, so a cut surface
    // and a filled one are pixel-identical everywhere the fill was visible; what goes is the
    // saturated core, which a drop shadow only ever showed through whatever it sits behind.
    //
    // The cut has a boundary of its OWN, offset by the record's cut vector, and that second
    // boundary is the whole of what makes a cast DIRECTIONAL: the falloff is measured from the
    // shadow's outline while the hole is taken against the caster's, so the near side is cut flush
    // against the caster while the far side reaches its full spread.  A zero offset is the shape
    // cutting itself.  Always the rounded box -- no other field carries this op.
    if ( f.shaped && ( g_ops & OP_CUT ) != 0u )
    {
        float4 soft = prim_row( 2u );
        if ( box_field( prim_local( px, prim_rect() ) - prim_row( 4u ).zw,
                        prim_rect().zw, prim_row( 1u ), soft.z ) <= 0.0 )
            cov = 0.0;
    }

    // INSET -- the inner shadow.  The falloff is re-measured INWARD from the boundary: full
    // strength against the edge, gone `aa` px in, and nothing outside it.  The second factor is the
    // ordinary 1 px edge band, so the outer rim antialiases exactly as the filled box's does rather
    // than stair-stepping.
    if ( f.shaped && ( g_ops & OP_INSET ) != 0u )
        cov = saturate( 1.0 + f.d / max( f.aa, 1e-4 ) ) * saturate( 0.5 - f.d );

    // PULSE -- breathe the coverage on the frame clock.  The one op here that needs no boundary,
    // so it reaches every field including a plain fill.  k is the depth reached, and every curve
    // but DECAY leaves it at 0 on the first frame -- a pulse fading in from nothing is not what
    // the shape shows before it has moved.
    if ( ( g_ops & OP_PULSE ) != 0u )
        cov *= 1.0 - prim_row( 3u ).x * g_k;

    // GRID -- the lattice, cut into whatever coverage the field produced.  Like PULSE it needs no
    // boundary, so it reaches every shape and a plain fill alike.
    if ( ( g_ops & OP_GRID ) != 0u )
        cov *= fx_grid_mul( px );

    return cov;
}

// The clip band: which clip rect cuts this fragment, resolved HERE rather than by the hardware
// scissor so a clip change never opens a new draw call.  The entry index is absolute within the
// frame's clip region; pc.clip_base is the region's origin, constant for the whole flush.  The
// table lives in a bindless storage buffer named by pc.clip_buf.  clip_buf 0 -- the reserved
// invalid slot -- means "no clip table bound": full coverage, so a pipeline user without a table
// (and the gui itself before its first upload) is never clipped by garbage.
//
// Entries are two float4s: [0] = (x0, y0, x1, y1) pixel EDGES, pre-snapped CPU-side to the same
// grid the hardware scissor was, so radius-0 coverage is bit-identical to the scissor it
// replaces (fragment centres sit at .5 against integer edges; no AA band on purpose -- a clip is
// a cut, not a shape).  radius > 0 evaluates the rounded-box field with a 1 px AA band instead:
// the clip a scissor rect could never express.
float clip_coverage( uint clip, float2 p )
{
    if ( pc.clip_buf == 0u )
        return 1.0;
    uint   e   = ( pc.clip_base + clip ) * 2u;
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
    // The record, and with it everything that used to be bit-packed into the vertex.  One 16-byte
    // load: a glyph, a flat fill and a debug-view quad all resolve here and never touch the rows
    // behind it.
    g_row       = ( pc.prim_base + i.prim ) * PRIM_ROWS;
    float4 head = u_buffers[ pc.prim_buf ][ g_row ];
    g_field     = asuint( head.x );
    g_ops       = asuint( head.y );
    g_tex       = asuint( head.z );

    // Placement, clip, the turn and the animation phase all come off the per-quad interpolants,
    // which is what frees the style to dedup across placements, angles and scroll regions.
    g_rect          = i.rect;
    g_rot           = i.inst.xy;
    g_phase         = i.inst.z;
    uint clip_entry = i.clip;

    // The animation clock, resolved once for every op that reads it.  anim_rate is cycles/sec off
    // the style and the phase is cycles off the quad, so the animation re-emits nothing -- only
    // pc.time moves -- and a staggered set still shares one style.  The curve then shapes the
    // phase; ops downstream see only g_k and never the clock.
    if ( ( g_ops & OP_ANIMATED ) != 0u )
    {
        float4 anim = prim_row( 5u );
        g_phi = frac( anim.x * pc.time + g_phase );
        g_k   = fx_curve( g_phi, asuint( anim.y ), anim.z );
    }

    // OP_SPIN turns the local frame through one revolution per cycle, composed into every
    // shape-local frame prim_local derives below.
    if ( ( g_ops & OP_SPIN ) != 0u )
    {
        float a = 6.28318531 * g_k;
        g_spin_c = cos( a );
        g_spin_s = sin( a );
    }

    // The clip cut applies to EVERYTHING, the debug views included -- the scissor this replaces
    // cut their geometry too, and a batch-view block that ignored its clip would lie about what
    // the draw painted.
    float ccov = clip_coverage( clip_entry, i.sv_pos.xy );

    // Debug views: bypass the atlas so geometry is visible regardless of glyph coverage.
    //   wireframe -- the LINE pipeline strokes triangle edges; a flat opaque color makes them
    //                show even across text quads (where s.r would otherwise alpha them away).
    //   batch     -- each draw call is pushed a distinct dbg_tint so its geometry reads as one
    //                solid color block; a color change marks a batch split.
    // The effect band is bypassed here too, on purpose: these views exist to show the geometry
    // actually submitted.
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

    /* The texture and its sampling model come off the RECORD (gui.h) rather than a draw call, so
       one draw can mix a coverage atlas, an SDF atlas and arbitrary RGBA images.
       NonUniformResourceIndex is mandatory here and not a formality: neighbouring primitives in a
       single draw legitimately name different descriptors, so the index is not wave-uniform and
       indexing without it is undefined.
       The SAMPLER is derived from the model rather than carried -- coverage must stay point-sampled
       (a filtered glyph atlas stops being crisp) and every other model filters.  Testing "not
       COVERAGE" means a model added later lands on the filtering side without touching this. */
    uint tex_mode = g_tex >> TEX_MODE_SHIFT;
    uint tex_slot = g_tex & TEX_INDEX_MASK;
    uint samp     = ( tex_mode == 0u ) ? pc.samp_point : pc.samp_image;

    /* GUI_OP_TILE_U: the stored U is normalized 0..1 (all UNORM16X2 can hold) and the repeat count
       rides the record.  Scaling HERE rather than in the vertex stage is exactly equivalent -- the
       multiply is affine and commutes with interpolation -- and it keeps the vertex shader a pure
       pass-through, so a dashed line is still one quad tiling the atlas stipple row under the
       sampler's REPEAT. */
    float2 uv = i.uv;
    if ( ( g_ops & OP_TILE_U ) != 0u )
        uv.x *= prim_row( 7u ).y;

    float4 s   = u_textures[ NonUniformResourceIndex( tex_slot ) ]
                     .Sample( u_samplers[ NonUniformResourceIndex( samp ) ], uv );
    float  cov = fx_coverage( i.sv_pos.xy ) * ccov;

    // SELF-SAMPLED primitives (GUI_OP_SELF): the shape is solid colour, so the texel is not
    // consulted (the sample above read a garbage location of a VALID texture, which is harmless and
    // cheaper than a divergent skip).  The flag is an OP, not a property of the field, so whether a
    // primitive is solid is the emit site's call: the same field fills solid here and carries real
    // texcoords through draw_texture_in.
    // Field 10 additionally sweeps the colour toward the record's second colour, lerped by the same
    // signed angle the aperture cut uses -- the gradient a 4-corner vertex colour cannot express.
    // Field 11 picks between the vertex colour and that same second colour by cell parity.
    float4 vcol = i.color;
    if ( ( g_ops & OP_SELF ) != 0u )
    {
        s = float4( 1.0, 1.0, 1.0, 1.0 );
        if ( g_field == 10u )
        {
            float4 parm = prim_row( 3u );
            float4 c2   = unpack_col( asuint( parm.w ) );
            float  ap   = parm.z;
            float2 sl   = prim_local( i.sv_pos.xy, prim_rect() ).yx;
            float  t    = saturate( ( atan2( sl.x, sl.y ) + ap )
                                    / max( 2.0 * ap, 1e-4 ) );
            vcol = lerp( vcol, c2, t );
        }
    }

    // OP_CHECKER -- alternate the fill with the record's pattern colour in cell-sized squares.  The
    // pitch and the parity phase come from row 7, the phase as a fraction of the TWO-cell colour
    // period (one cell of phase would simply swap the colours).  Parity is floor-based like every
    // pattern mod here, since fmod truncates.  As an op it lands on whatever shape the field drew,
    // so the transparency chequerboard behind a swatch is the swatch's own rounded rect.
    if ( ( g_ops & OP_CHECKER ) != 0u )
    {
        float4 pat  = prim_row( 7u );
        float  cell = pat.x;
        float2 ph   = unpack_unorm16x2( asuint( pat.z ) ) * ( 2.0 * cell );
        float2 t    = ( i.sv_pos.xy - ph ) / cell;
        float  k    = floor( t.x ) + floor( t.y );
        if ( k - 2.0 * floor( k * 0.5 ) >= 0.5 )
            vcol = unpack_col( asuint( pat.w ) );
    }

    // GUI_OP_GRAD -- the ramp from the fill's own colour toward the record's second one.  Resolved
    // here rather than by the vertices because two of its three shapes have no vertex expression at
    // all: colours at a rectangle's four corners can describe a linear ramp and nothing else.  The
    // ramp works in the shape's own local frame, so it turns with the shape and spans it exactly --
    // where corner colours would be sampled out on the falloff skirt and arrive stretched by it.
    if ( ( g_ops & OP_GRAD ) != 0u )
    {
        float4 grect = prim_rect();
        float2 lp    = prim_local( i.sv_pos.xy, grect );
        float2 g     = prim_row( 4u ).xy;
        float  t;

        if ( ( g_ops & OP_GRAD_RADIAL ) != 0u )
            t = saturate( length( lp / max( grect.zw, float2( 1e-4, 1e-4 ) ) ) );
        else if ( ( g_ops & OP_GRAD_CONIC ) != 0u )
            // Angular, MIRRORED about the axis: full at the axis, gone at the far side, and the
            // same on the way back.  A conic ramp that wrapped instead would meet itself at the
            // axis with col_b against the fill colour and no transition between them -- a hard
            // light/dark edge sitting in open space.  This one depends on |angle| alone, which is
            // continuous across the far side, so there is no seam anywhere on the shape.
            t = 1.0 - abs( atan2( g.x * lp.y - g.y * lp.x, dot( lp, g ) ) ) / PI;
        else
            // g arrives already divided by the shape's extent along it (gui.h), so the whole ramp
            // is one dot product no matter what angle it runs at.
            t = saturate( dot( lp, g ) + 0.5 );

        // The midpoint bend: grad_mid is the EXPONENT the emit site mapped the authored 0..1
        // midpoint to (ln 0.5 / ln mid), so t = 0.5 lands where the author put it.  0 means the
        // linear default and skips the pow.
        float e = prim_row( 5u ).w;
        if ( e > 0.0 )
            t = pow( t, e );

        vcol = lerp( vcol, unpack_col( asuint( prim_row( 3u ).w ) ), t );
    }

    /* THE TEXEL, FOLDED IN.  Each sampling model contributes to the SAME colour and alpha rather
       than returning its own pixel, which is what lets everything downstream -- the ramp above,
       the frame below, the dither at the end -- reach text and images too.  Each model is compared
       exactly rather than by "non-zero", so a model added later lands on the coverage path only if
       that is what it actually wants. */
    float alpha = vcol.a;

    if ( tex_mode == 1u )
    {
        // GUI_TEX_RGBA -- full-RGBA image (scene viewport / arbitrary bindless texture): the texel
        // IS the colour, with the fill colour acting as a tint.  The texel arrives LINEAR:
        // _SRGB-format textures are hardware-decoded at sample time and UNORM render targets hold
        // linear data.  The tint is linear too (decoded in the vertex stage), so both sides of this
        // multiply are light and the product means something.
        vcol.rgb *= s.rgb;
        alpha    *= s.a;
    }
    else if ( tex_mode == 2u )
    {
        // GUI_TEX_SDF -- distance-field text.  The texel is not coverage: 128/255 is exactly ON the
        // outline, above is inside, below is outside (orb_font.h).  Coverage is recovered from the
        // SCREEN-SPACE DERIVATIVE of that field, which is the whole reason this mode exists:
        // fwidth(d) is how much the distance changes across one pixel HERE, so d/fwidth(d) is the
        // distance to the edge measured in pixels no matter how the quad was scaled or rotated.
        // The AA band is therefore always one pixel wide, and no per-vertex or per-draw parameter
        // has to carry the scale -- which is why an SDF font costs the vertex format nothing.
        // The max() guards the degenerate case: deep inside or far outside the field is flat,
        // fwidth is 0, and d/0 would be a NaN rather than the saturated 1 or 0 that is wanted.
        float d    = s.r - ( 128.0 / 255.0 );
        float dpx  = d / max( fwidth( d ), 1e-6 );        // signed distance to the edge, in PIXELS
        float fill = clamp( dpx + 0.5, 0.0, 1.0 );

        // GUI_OP_TEXT_EDGE -- a second colour outside the glyph boundary (Slate's SecondaryColor,
        // without Slate's vertex field).  Because dpx is already a pixel distance, "outline" is
        // just the same threshold moved out by `width`: one extra clamp on a number this branch had
        // to compute anyway, from the ONE texture sample it had already taken.  No second quad, no
        // offset copy of the run, no batch split -- which is what makes it affordable on body text.
        if ( ( g_ops & OP_TEXT_EDGE ) != 0u )
        {
            float4 pat  = prim_row( 7u );
            float  wpx  = pat.y;
            float4 ecol = unpack_col( asuint( pat.w ) );

            // Source-over of the fill onto the band, resolved analytically: the band contributes
            // only where the fill does not (1 - fill), so the seam between them is antialiased by
            // the same coverage that antialiases the glyph, and the two never double-darken.
            float outer = clamp( dpx + wpx + 0.5, 0.0, 1.0 );
            float af    = alpha * fill;
            float ao    = ecol.a * outer * ( 1.0 - fill );
            float at    = af + ao;
            vcol.rgb = ( vcol.rgb * af + ecol.rgb * ao ) / max( at, 1e-6 );
            alpha    = at;
        }
        else
        {
            alpha *= fill;
        }
    }
    else
    {
        // The coverage atlas: s.r is the glyph's coverage, and 1.0 for the white solid-colour texel
        // and for every self-sampled field, so non-text draws pass straight through.
        alpha *= s.r;
    }

    // Coverage folds in here, once, for every model.
    alpha *= cov;

    // GUI_OP_FRAME -- the border band, source-over the fill, in this one quad.  The band's coverage
    // came from fx_coverage off the same field the fill resolved; the algebra is TEXT_EDGE's
    // analytic source-over, so the result is pixel-identical to the fill + outline pair it
    // replaces: the fill contributes only where the band's alpha does not, and the shared boundary
    // antialiases once instead of double-darkening.  The clip cut scales the band directly because
    // `alpha` already carries it for the fill.
    if ( ( g_ops & OP_FRAME ) != 0u )
    {
        float4 bcol = i.col2;
        float  ab   = bcol.a * g_frame_band * ccov;
        float  af   = alpha * ( 1.0 - ab );
        float  at   = ab + af;
        vcol.rgb = ( bcol.rgb * ab + vcol.rgb * af ) / max( at, 1e-6 );
        alpha    = at;
    }

    // vcol.rgb is linear light throughout (the fill colour arrived decoded from the vertex stage,
    // and every stage above worked in that space); alpha is coverage, which was linear all along.
    float4 outc = float4( vcol.rgb, alpha );

    // GUI_OP_DITHER -- half an 8-bit step of screen-space noise on the output, so a wide soft
    // ramp (a drop shadow's falloff, a gradient across a panel) quantizes to distinct pixels
    // instead of visible bands.  Interleaved gradient noise: one uncorrelated fraction per pixel,
    // stable per position, no texture.  Applied to alpha too -- shadow banding IS alpha banding.
    if ( ( g_ops & OP_DITHER ) != 0u )
    {
        float n = frac( 52.9829189 * frac( dot( i.sv_pos.xy,
                                                float2( 0.06711056, 0.00583715 ) ) ) );
        outc += ( n - 0.5 ) * ( 1.0 / 255.0 );
    }
    return outc;
}
