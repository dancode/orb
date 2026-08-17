// qp_pull.vs.hlsl -- the PULLED vertex stage of the sb_quad_pull proof: no vertex buffer at
// all.  The pipeline binds nothing; the draw is cmd_draw of 6 * N bare vertices, and this stage
// computes which quad and which corner it is from SV_VertexID, fetches the 48-byte quad record
// (gui.h, gui_quad_t) from the bindless storage-buffer array, and expands the corner position
// itself.  Cooked to bin/shaders/qp_pull.vs.oshd by the 'shader' lines on the sb_quad_pull
// target; qp_vb.vs.hlsl is the classic vertex-buffer stage it is measured against.
//
// The push constant block mirrors qp_push_t in sb_quad_pull.c; keep the three shaders and the
// C struct in step.

struct qp_pc_t
{
    float2 scale;        // pixel -> NDC scale (2/w, 2/h); Vulkan clip space, +y down
    float2 offset;       // pixel -> NDC offset (-1, -1)
    uint   quad_buf;     // bindless buffer slot of the quad records
    uint   quad_base;    // first record of this draw
    uint   style_buf;    // bindless buffer slot of the style records
    uint   style_base;   // first style of this draw
};
[[vk::push_constant]] qp_pc_t pc;

// The bindless storage-buffer array (set 0, binding 2) -- the same binding the gui fragment
// reads its record tables through, declared float4[] because that is the one element type the
// array can have.  Integer lanes come back through asuint, a reinterpret, not a convert.
[[vk::binding( 2, 0 )]] StructuredBuffer<float4> u_buffers[] : register( t0, space1 );

#define QUAD_ROWS   3u   // gui.h GUI_QUAD_ROWS: cx/cy/hw/hh | uv0/uv1/abgr/style | clip/flags
#define STYLE_ROWS  8u   // gui.h GUI_PRIM_ROWS: a style is a gui_prim_t with placement dead

// Corner of the two-triangle quad, in (0,0)..(1,1): triangles 0-1-2 and 0-2-3 of the corner
// ring TL, TR, BR, BL.
static const float2 k_corner[ 6 ] = {
    float2( 0.0, 0.0 ), float2( 1.0, 0.0 ), float2( 1.0, 1.0 ),
    float2( 0.0, 0.0 ), float2( 1.0, 1.0 ), float2( 0.0, 1.0 ),
};

struct vs_out_t
{
    float4                sv_pos : SV_Position;
    float4                color  : COLOR0;
    float2                uv     : TEXCOORD0;
    nointerpolation uint  style  : TEXCOORD1;
};

float2 unpack_unorm16x2( uint p )
{
    return float2( p & 0xFFFFu, p >> 16u ) / 65535.0;
}

float4 unpack_unorm8x4( uint c )
{
    return float4( c & 0xFFu, ( c >> 8u ) & 0xFFu, ( c >> 16u ) & 0xFFu, c >> 24u ) / 255.0;
}

vs_out_t main( uint vid : SV_VertexID )
{
    uint   quad   = vid / 6u;
    float2 corner = k_corner[ vid % 6u ];

    uint   row = ( pc.quad_base + quad ) * QUAD_ROWS;
    float4 r0  = u_buffers[ pc.quad_buf ][ row ];        // cx, cy, hw, hh -- the TRUE extents
    float4 r1  = u_buffers[ pc.quad_buf ][ row + 1u ];   // uv0, uv1, abgr, style

    // The expansion pad comes from the STYLE, not the quad: feather (style row 3 leads with it)
    // plus a one-pixel AA guard.  This dependent fetch is the cost under measurement -- the
    // real replay backend pays exactly this to keep pre-inflated extents out of the record.
    uint   style   = asuint( r1.w );
    float4 srow    = u_buffers[ pc.style_buf ][ ( pc.style_base + style ) * STYLE_ROWS + 3u ];
    float  pad     = srow.x + 1.0;

    float2 sgn = corner * 2.0 - 1.0;
    float2 p   = r0.xy + sgn * ( r0.zw + pad );

    vs_out_t o;
    o.sv_pos   = float4( p * pc.scale + pc.offset, 0.0, 1.0 );
    o.sv_pos.y = -o.sv_pos.y;   // cancel the cook's -fvk-invert-y: pc maps +y down already
    o.color    = unpack_unorm8x4( asuint( r1.z ) );
    o.uv       = lerp( unpack_unorm16x2( asuint( r1.x ) ), unpack_unorm16x2( asuint( r1.y ) ), corner );
    o.style    = style;
    return o;
}
