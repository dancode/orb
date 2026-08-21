// gui_quad.vs.hlsl -- the QUAD-RECORD pipeline's vertex stage: no vertex buffer at all.  A draw
// is cmd_draw of 6 * N bare vertices; this stage computes quad = SV_VertexID / 6 and corner =
// SV_VertexID % 6 (VertexIndex includes firstVertex under Vulkan, and every draw's firstVertex
// is a multiple of 6), fetches the 16-byte quad record (gui.h, gui_quad_t) from the bindless
// array in ONE load, and expands the covering corner itself.  Cooked to
// bin/shaders/gui_quad.vs.oshd.
//
// What the expansion does per quad, keyed by the record's flags (gui.h, GUI_QUAD_RULE_*):
//   EXACT    the stored half-extents, rotated by the quad's own (cos, sin)
//   SKIRT    grown by the SDF falloff pad -- feather/2 + 1 -- from the style's row 2
//   CAPSULE  hw is the half-length and hh the radius: the along axis grows by hh as well,
//            so the round caps are covered
//   BBOX     the stored extents ARE the covering (pad baked by the tessellator), expanded
//            axis-aligned -- the arc family, whose local frame is a reflection this rotation
//            could not reproduce
// The rotation is applied for the first three (identity in most styles).
//
// A quad under the BAND tag ignores the rule and takes one strip of the frame around the region
// its field leaves at zero coverage -- see band_local below.  Its placement is the shape's, exactly
// as the single quad it replaces, so the fragment cannot tell the difference.
//
// The mvp is authored in VULKAN clip space; shader_tool bakes -fvk-invert-y into every
// vertex-stage cook, so this shader negates y once to cancel it.

#include "gui_common.hlsli"

// Corner of the two-triangle quad in (0,0)..(1,1): triangles 0-1-2 / 0-2-3 of the ring
// TL, TR, BR, BL -- the winding every legacy quad used (the pipeline does not cull).
static const float2 k_corner[ 6 ] = {
    float2( 0.0, 0.0 ), float2( 1.0, 0.0 ), float2( 1.0, 1.0 ),
    float2( 0.0, 0.0 ), float2( 1.0, 1.0 ), float2( 0.0, 1.0 ),
};

// The BAND covering (gui.h, tag BAND): the local-frame corner of one of four quads that tile the
// frame between the shape's padded outer rect and the rect its field provably leaves at zero
// coverage.  A shape that only paints near its boundary -- a drop-shadow skirt, a thin outline, an
// inner shadow -- otherwise rasterizes its whole middle for nothing.
//
// The HOLE is stated as a depth inward from the boundary, plus a centre offset:
//   CUT    coverage is zero wherever the CASTER's box contains the fragment, and the caster is
//          this shape's own extents moved by the record's cut vector.  Depth 0, offset applied.
//   BAND   the field becomes abs(d + w/2) - w/2 and resolves over `feather`, so it is zero once
//          d <= -(border + feather/2).
//   INSET  coverage carries a saturate(1 + d/feather) factor, zero at d <= -feather.
// The tessellator only tags a shape carrying EXACTLY ONE of them, since two would measure from
// different boundaries and describe a hole neither formula states; the order here is the tie-break
// that keeps this well defined regardless.
//
// The corner radius is taken out of the hole so the rounded corners stay inside the bands.  The
// inscribed offset for a circular corner is r * (1 - 1/sqrt2); a corner_pow above 2 bulges the
// arc OUTWARD toward its square, so the circular figure stays conservative for every profile.
//
// Everything here is CLAMPED rather than trusted: the hole is forced non-negative and its centre
// forced inside the outer rect, so the four bands tile with no gap and no overlap whatever the
// record holds.  The tessellator decides only whether four quads beat one.
float2 band_local( float2 corner, uint band, float2 he0, float pad, uint style )
{
    uint   base = style_row( style );
    uint   ops  = asuint( u_buffers[ pc.prim_buf ][ base ].y );
    float4 rad  = u_buffers[ pc.prim_buf ][ base + 1u ];
    float4 edge = u_buffers[ pc.prim_buf ][ base + 2u ];

    float  depth = 0.0;
    float2 ctr   = float2( 0.0, 0.0 );

    if ( ( ops & OP_CUT ) != 0u )
        ctr = u_buffers[ pc.prim_buf ][ base + 4u ].zw;
    else if ( ( ops & OP_BAND ) != 0u )
        depth = edge.y + edge.x * 0.5;
    else if ( ( ops & OP_INSET ) != 0u )
        depth = edge.x;

    float rmax = max( max( rad.x, rad.y ), max( rad.z, rad.w ) );

    float2 ho = he0 + pad;
    float2 hi = max( he0 - ( depth + 0.29289322 * rmax ), float2( 0.0, 0.0 ) );
    ctr       = clamp( ctr, -( ho - hi ), ho - hi );

    float2 lo = ctr - hi, up = ctr + hi;

    // Pinwheel: full-width top and bottom, then the two side strips between them.
    float4 b;
    if      ( band == 0u ) b = float4( -ho.x, -ho.y,  ho.x,  lo.y );
    else if ( band == 1u ) b = float4( -ho.x,  up.y,  ho.x,  ho.y );
    else if ( band == 2u ) b = float4( -ho.x,  lo.y,  lo.x,  up.y );
    else                   b = float4(  up.x,  lo.y,  ho.x,  up.y );

    return lerp( b.xy, b.zw, corner );
}

// MUST be declared in the same ORDER as ps_in_t (gui_fx.hlsli), field for field.  Vulkan matches
// stage interfaces by LOCATION, and dxc assigns locations in declaration order -- the semantic
// numbers are not what pairs them up.  A field inserted here but appended there shifts every
// interpolant after it by one slot and the fragment reads its neighbours' data.
struct vs_out_t
{
    float4                 sv_pos : SV_Position;
    float4                 color  : COLOR0;
    float2                 uv     : TEXCOORD0;
    nointerpolation uint   prim   : TEXCOORD1;   // style record index, slot-local
    nointerpolation float4 rect   : TEXCOORD2;   // shape placement: centre + stored half-extents
    nointerpolation uint   clip   : TEXCOORD3;   // clip-table entry index, slot-local
    nointerpolation float4 border : TEXCOORD4;   // GUI_OP_FRAME: the border band's colour --
                                                  //   an instance lane, never the style
    nointerpolation float4 inst   : TEXCOORD5;   // per-instance lanes off the quad:
                                                  //   xy = the turn (cos, sin), z = animation
                                                  //   phase in cycles, w = swell amplitude px
    nointerpolation uint   tag    : TEXCOORD6;   // GUI_QUAD_TAG_* in bits 0-1, "the SDF atlas"
                                                  //   in bit 2.  A GLYPH names no style record,
                                                  //   so the fragment takes its texture from the
                                                  //   push block instead of fetching one
};

vs_out_t main( uint vid : SV_VertexID )
{
    uint   quad   = vid / 6u;
    float2 corner = k_corner[ vid % 6u ];

    float4 q = u_buffers[ pc.quad_buf ][ ( pc.quad_base + quad ) * QUAD_ROWS ];

    // Placement is quarter-pixel fixed point (gui.h): a signed centre and an unsigned half-extent
    // packed two to a lane.  The signed halves are sign-extended by shifting them up to the top of
    // an int and arithmetic-shifting back down.
    uint  cw = asuint( q.x ), ew = asuint( q.y );
    int2  ci = int2( asint( cw << 16 ) >> 16, asint( cw ) >> 16 );
    float4 q0 = float4( float2( ci ), float2( ew & 0xFFFFu, ew >> 16u ) ) * 0.25;

    // The index word is a tagged union (gui.h): a whole GLYPH carries an atlas ID where every
    // other quad carries a style record; a STYLED glyph carries both -- the ID where GLYPH keeps
    // it and a style where GLYPH keeps fx bits, so its ops ride a record while its rect stays
    // repack-stable.
    uint idx      = asuint( q.w );
    uint tag      = idx >> GUI_QUAD_TAG_SHIFT;
    bool isglyph  = ( tag == GUI_QUAD_TAG_GLYPH );
    bool isstyled = ( tag == GUI_QUAD_TAG_GSTYLED );
    bool isband   = ( tag == GUI_QUAD_TAG_BAND );
    bool anyglyph = isglyph || isstyled;

    uint style = isglyph  ? 0u
               : isstyled ? ( ( idx >> GUI_QUAD_GSTYLE_SHIFT ) & GUI_QUAD_GSTYLE_MASK )
                          : ( ( idx >> GUI_QUAD_STYLE_SHIFT  ) & GUI_QUAD_STYLE_MASK  );

    // A glyph is EXACT, styled or not; a band is always SKIRT, which is what frees its rule bits
    // to name the band.
    uint rule = anyglyph ? 0u : ( isband ? 1u : ( ( idx >> GUI_QUAD_RULE_SHIFT ) & 3u ) );

    // The instance extras: turn, animation phase, border colour.  Most quads name no record and
    // take the all-zero row, which IS the default -- an unrotated shape in step with the clock and
    // no border band (gui.h, gui_fx_t).  A STYLED glyph has no fx bits at all: its layout spent
    // them on the style, so a rotated styled run takes the SHAPED tag instead.
    uint   fxrow = isstyled ? 0u
                 : isglyph  ? ( ( idx >> GUI_QUAD_GFX_SHIFT ) & GUI_QUAD_GFX_MASK )
                            : ( ( idx >> GUI_QUAD_FX_SHIFT  ) & GUI_QUAD_FX_MASK  );
    float4 fxr   = fx_record( fxrow, 0u );

    // The shape's TURN, per instance -- a unit (cos, sin) packed like a uv pair, with the all-zero
    // word reserved for identity (gui.h, gui_xform_pack).  This is the frame every field works in,
    // a CAPSULE's direction included: one style serves a whole polyline because the direction was
    // never in the style to begin with.
    uint   xw = asuint( fxr.x );
    float2 rt = ( xw == 0u ) ? float2( 1.0, 0.0 )
                             : normalize( unpack_unorm16x2( xw ) * 2.0 - 1.0 );

    // The one style fetch the expansion needs: row 2 leads with the feather the SDF pad derives
    // from.  Cache-hot -- a window's quads share a handful of styles -- and skipped entirely for
    // the rules that take no pad, which is every glyph.
    //
    // The instance's swell amplitude (gui_fx_t.swell, GUI_OP_SWELL) grows the pad too: the
    // boundary travels up to that far past the authored rect, and the covering has to be there
    // when it arrives.  Unconditional -- the lane is 0.0 on every record that does not swell,
    // and a shrink (negative) needs no room.
    float pad = 0.0;
    if ( rule == 1u || rule == 2u )
        pad = u_buffers[ pc.prim_buf ][ style_row( style ) + 2u ].x * 0.5 + 1.0
            + max( fxr.w, 0.0 );

    float2 he = q0.zw;
    if ( rule == 2u )
        he.x += q0.w;          // capsule: the caps bulge half a thickness past each endpoint
    he += pad;

    // The covering corner, in the shape's own frame.  A BAND quad takes one strip of the frame
    // around the hole instead of the whole rect; everything else spans `he`.
    float2 lp = isband ? band_local( corner, ( idx >> GUI_QUAD_BAND_SHIFT ) & 3u, q0.zw, pad, style )
                       : ( corner * 2.0 - 1.0 ) * he;
    float2 wp = ( rule == 3u )
              ? q0.xy + lp     // BBOX: pre-baked covering, axis-aligned
              : q0.xy + float2( lp.x * rt.x - lp.y * rt.y, lp.x * rt.y + lp.y * rt.x );

    // The uv rect, which the quad record has no lane for at all.  A whole glyph names a
    // glyph-table entry inside the index word -- the indirection that lets cached text outlive an
    // atlas repack, since the table rewrites in place and the ID does not move.  Everything that
    // samples a texture without being a glyph -- icons, sprites, a dashed line's stipple row, the
    // narrowed rect a glyph cut to its window carries -- takes it from row B of its instance
    // record.  A flat fill names no record and reads the zero rect it never samples.
    uint2 uvw = anyglyph ? glyph_uv( ( idx >> GUI_QUAD_GLYPH_SHIFT ) & GUI_QUAD_GLYPH_MASK )
                         : asuint( fx_record( fxrow, 1u ).xy );
    float2 uv0 = unpack_unorm16x2( uvw.x );
    float2 uv1 = unpack_unorm16x2( uvw.y );

    // A SKIRT quad's uv was authored over the TRUE extents; the corners sit `pad` outside them,
    // so the span scales out by the same ratio and clamps at the rect -- reproducing the vertex
    // backend's grown-corner mapping (linear map, clamped), so a textured rounded quad shows its
    // picture at authored size with the edge texels held across the falloff skirt.
    if ( rule == 1u )
    {
        float2 uvc  = ( uv0 + uv1 ) * 0.5;
        float2 grow = ( q0.zw + pad ) / max( q0.zw, float2( 1e-4, 1e-4 ) );
        float2 lo   = min( uv0, uv1 ), hi = max( uv0, uv1 );
        uv0 = clamp( uvc + ( uv0 - uvc ) * grow, lo, hi );
        uv1 = clamp( uvc + ( uv1 - uvc ) * grow, lo, hi );
    }

    // Where this corner sits across the covering, 0..1 -- what the uv span is interpolated over.
    // It IS `corner` for a quad that spans its whole rect, and is recovered from the local position
    // for a BAND, whose corners land wherever the frame put them.  Selected rather than always
    // recovered so a glyph's uv stays bit-exact against a point-sampled atlas.
    float2 t = isband ? ( lp / max( he, float2( 1e-4, 1e-4 ) ) * 0.5 + 0.5 ) : corner;

    float4 col = unpack_col( asuint( q.z ) );    // sRGB -> linear, alpha untouched (coverage)

    vs_out_t o;
    o.sv_pos   = mul( pc.mvp, float4( wp, 0.0, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    o.color    = col;
    o.uv       = lerp( uv0, uv1, t );
    o.prim     = style;
    o.rect     = q0;
    o.clip     = ( idx >> GUI_QUAD_CLIP_SHIFT ) & GUI_QUAD_CLIP_MASK;
    o.tag      = tag | ( ( anyglyph && ( idx & GUI_QUAD_SDF_BIT ) != 0u ) ? 4u : 0u );
    o.border   = unpack_col( asuint( fxr.z ) );
    o.inst     = float4( rt, asuint( fxr.y ) / 65535.0, fxr.w );
    return o;
}
