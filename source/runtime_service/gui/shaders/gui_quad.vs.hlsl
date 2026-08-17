// gui_quad.vs.hlsl -- the QUAD-RECORD pipeline's vertex stage: no vertex buffer at all.  A draw
// is cmd_draw of 6 * N bare vertices; this stage computes quad = SV_VertexID / 6 and corner =
// SV_VertexID % 6 (VertexIndex includes firstVertex under Vulkan, and every draw's firstVertex
// is a multiple of 6), fetches the 48-byte quad record (gui.h, gui_quad_t) from the bindless
// array, and expands the covering corner itself.  Cooked to bin/shaders/gui_quad.vs.oshd.
//
// What the expansion does per quad, keyed by the record's flags (gui.h, GUI_QUAD_RULE_*):
//   EXACT    the stored half-extents, rotated by the style's (rot_cos, rot_sin)
//   SKIRT    grown by the SDF falloff pad -- feather/2 + 1 -- from the style's row 3
//   CAPSULE  hw is the half-length and hh the radius: the along axis grows by hh as well,
//            so the round caps are covered
//   BBOX     the stored extents ARE the covering (pad baked by the tessellator), expanded
//            axis-aligned -- the arc family, whose local frame is a reflection this rotation
//            could not reproduce
// The rotation is applied for the first three (identity in most styles); glyph-flagged quads
// resolve their uv rect from the glyph table by stable id, with an optional horizontal cut pair
// for clipped straddlers.
//
// The mvp is authored in VULKAN clip space; shader_tool bakes -fvk-invert-y into every
// vertex-stage cook, so this shader negates y once to cancel it.

#include "gui_common.hlsli"

#define QUAD_ROWS   3u    // gui.h GUI_QUAD_ROWS
#define STYLE_ROWS  7u    // gui.h GUI_PRIM_ROWS -- a style is a gui_prim_t

// Corner of the two-triangle quad in (0,0)..(1,1): triangles 0-1-2 / 0-2-3 of the ring
// TL, TR, BR, BL -- the winding every legacy quad used (the pipeline does not cull).
static const float2 k_corner[ 6 ] = {
    float2( 0.0, 0.0 ), float2( 1.0, 0.0 ), float2( 1.0, 1.0 ),
    float2( 0.0, 0.0 ), float2( 1.0, 1.0 ), float2( 0.0, 1.0 ),
};

struct vs_out_t
{
    float4                 sv_pos : SV_Position;
    float4                 color  : COLOR0;
    float2                 uv     : TEXCOORD0;
    nointerpolation uint   prim   : TEXCOORD1;   // style record index, slot-local
    nointerpolation float4 rect   : TEXCOORD2;   // shape placement: centre + stored half-extents
    nointerpolation uint   clip   : TEXCOORD3;   // clip-table entry index, region-absolute
};

float2 unpack_unorm16x2( uint p )
{
    return float2( p & 0xFFFFu, p >> 16u ) / 65535.0;
}

vs_out_t main( uint vid : SV_VertexID )
{
    uint   quad   = vid / 6u;
    float2 corner = k_corner[ vid % 6u ];

    uint   row = ( pc.quad_base + quad ) * QUAD_ROWS;
    float4 q0  = u_buffers[ pc.quad_buf ][ row ];        // cx, cy, hw, hh
    float4 q1  = u_buffers[ pc.quad_buf ][ row + 1u ];   // uv0, uv1, abgr, style
    float4 q2  = u_buffers[ pc.quad_buf ][ row + 2u ];   // clip, flags, cut, reserved

    uint style = asuint( q1.w );
    uint flags = asuint( q2.y );
    uint rule  = flags & 3u;

    // The one style fetch the expansion needs: row 2 leads with feather and carries the shape's
    // rotation.  Cache-hot -- a window's quads share a handful of styles.
    float4 s3 = u_buffers[ pc.prim_buf ][ ( pc.prim_base + style ) * STYLE_ROWS + 2u ];

    // The shape's turn.  A zeroed rot pair reads as IDENTITY -- the zero-when-unused record rule:
    // a plain fill's style never wrote the lanes.  A CAPSULE's direction rides the quad's own uv
    // lanes instead of the style (one style serves a whole polyline), so it overrides here.
    float2 rt = ( s3.z == 0.0 && s3.w == 0.0 ) ? float2( 1.0, 0.0 ) : s3.zw;
    if ( rule == 2u )
        rt = normalize( unpack_unorm16x2( asuint( q1.x ) ) * 2.0 - 1.0 );

    float pad = ( rule == 1u || rule == 2u ) ? s3.x * 0.5 + 1.0 : 0.0;

    float2 he = q0.zw;
    if ( rule == 2u )
        he.x += q0.w;          // capsule: the caps bulge half a thickness past each endpoint
    he += pad;

    float2 lp = ( corner * 2.0 - 1.0 ) * he;
    float2 wp = ( rule == 3u )
              ? q0.xy + lp     // BBOX: pre-baked covering, axis-aligned
              : q0.xy + float2( lp.x * rt.x - lp.y * rt.y, lp.x * rt.y + lp.y * rt.x );

    // The uv rect: packed corners, or -- under the glyph flag -- the glyph table entry the quad
    // names by stable id, so an atlas repack rewrites the table and never the quad.  The cut pair
    // narrows the rect horizontally for clip-straddling glyphs (0 = the whole glyph).
    float2 uv0, uv1;
    if ( ( flags & 4u ) != 0u )
    {
        float4 ge  = u_buffers[ pc.glyph_buf ][ asuint( q1.x ) ];
        uint   cw  = asuint( q2.z );
        float2 cut = ( cw == 0u ) ? float2( 0.0, 1.0 ) : unpack_unorm16x2( cw );
        uv0 = float2( lerp( ge.x, ge.z, cut.x ), ge.y );
        uv1 = float2( lerp( ge.x, ge.z, cut.y ), ge.w );
    }
    else
    {
        uv0 = unpack_unorm16x2( asuint( q1.x ) );
        uv1 = unpack_unorm16x2( asuint( q1.y ) );
    }

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

    float4 col = unpack_col( asuint( q1.z ) );   // sRGB -> linear, alpha untouched (coverage)

    vs_out_t o;
    o.sv_pos   = mul( pc.mvp, float4( wp, 0.0, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    o.color    = col;
    o.uv       = lerp( uv0, uv1, corner );
    o.prim     = style;
    o.rect     = q0;
    o.clip     = asuint( q2.x );
    return o;
}
