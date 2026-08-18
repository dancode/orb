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
// The mvp is authored in VULKAN clip space; shader_tool bakes -fvk-invert-y into every
// vertex-stage cook, so this shader negates y once to cancel it.

#include "gui_common.hlsli"

// Corner of the two-triangle quad in (0,0)..(1,1): triangles 0-1-2 / 0-2-3 of the ring
// TL, TR, BR, BL -- the winding every legacy quad used (the pipeline does not cull).
static const float2 k_corner[ 6 ] = {
    float2( 0.0, 0.0 ), float2( 1.0, 0.0 ), float2( 1.0, 1.0 ),
    float2( 0.0, 0.0 ), float2( 1.0, 1.0 ), float2( 0.0, 1.0 ),
};

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
                                                  //   phase in cycles, w unused
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
    // other quad carries a style record, and neither has to make room for the other.
    uint idx     = asuint( q.w );
    uint tag     = idx >> GUI_QUAD_TAG_SHIFT;
    bool isglyph = ( tag == GUI_QUAD_TAG_GLYPH );

    uint style = isglyph ? 0u : ( ( idx >> GUI_QUAD_STYLE_SHIFT ) & GUI_QUAD_STYLE_MASK );
    uint rule  = isglyph ? 0u : ( ( idx >> GUI_QUAD_RULE_SHIFT ) & 3u );   // a glyph is EXACT

    // The instance extras: turn, animation phase, border colour.  Most quads name no record and
    // take the all-zero row, which IS the default -- an unrotated shape in step with the clock and
    // no border band (gui.h, gui_fx_t).
    uint   fxrow = isglyph ? ( ( idx >> GUI_QUAD_GFX_SHIFT ) & GUI_QUAD_GFX_MASK )
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
    float pad = 0.0;
    if ( rule == 1u || rule == 2u )
        pad = u_buffers[ pc.prim_buf ][ ( pc.prim_base + style ) * PRIM_ROWS + 2u ].x * 0.5 + 1.0;

    float2 he = q0.zw;
    if ( rule == 2u )
        he.x += q0.w;          // capsule: the caps bulge half a thickness past each endpoint
    he += pad;

    float2 lp = ( corner * 2.0 - 1.0 ) * he;
    float2 wp = ( rule == 3u )
              ? q0.xy + lp     // BBOX: pre-baked covering, axis-aligned
              : q0.xy + float2( lp.x * rt.x - lp.y * rt.y, lp.x * rt.y + lp.y * rt.x );

    // The uv rect, which the quad record has no lane for at all.  A whole glyph names a
    // glyph-table entry inside the index word -- the indirection that lets cached text outlive an
    // atlas repack, since the table rewrites in place and the ID does not move.  Everything that
    // samples a texture without being a glyph -- icons, sprites, a dashed line's stipple row, the
    // narrowed rect a glyph cut to its window carries -- takes it from row B of its instance
    // record.  A flat fill names no record and reads the zero rect it never samples.
    uint2 uvw = isglyph ? glyph_uv( ( idx >> GUI_QUAD_GLYPH_SHIFT ) & GUI_QUAD_GLYPH_MASK )
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

    float4 col = unpack_col( asuint( q.z ) );    // sRGB -> linear, alpha untouched (coverage)

    vs_out_t o;
    o.sv_pos   = mul( pc.mvp, float4( wp, 0.0, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    o.color    = col;
    o.uv       = lerp( uv0, uv1, corner );
    o.prim     = style;
    o.rect     = q0;
    o.clip     = ( idx >> GUI_QUAD_CLIP_SHIFT ) & GUI_QUAD_CLIP_MASK;
    o.tag      = tag | ( ( isglyph && ( idx & GUI_QUAD_SDF_BIT ) != 0u ) ? 4u : 0u );
    o.border   = unpack_col( asuint( fxr.z ) );
    o.inst     = float4( rt, asuint( fxr.y ) / 65535.0, 0.0 );
    return o;
}
