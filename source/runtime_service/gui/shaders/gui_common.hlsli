// gui_common.hlsli -- what both gui shader stages share: the push-constant block, the bindless
// storage-buffer array, and the colour decode.  Included by gui_quad.vs.hlsl and
// gui_quad.ps.hlsl (via gui_fx.hlsli); the build's shader cook treats a sibling .hlsli edit as
// staleness, so editing here recooks both.
//
// The push block mirrors gui_push_t in gui_render_init.c.

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
    uint     prim_buf;     // bindless buffer slot of the style records
    uint     prim_base;    // this window SLOT's first record (records, not float4s)
    uint     quad_buf;     // bindless buffer slot of the quad records (gui_quad_t)
    uint     quad_base;    // the flush's quad-region origin (quads, not float4s)
    uint     glyph_buf;    // bindless buffer slot of the glyph UV table (gui_glyph_uv_t).  No base
                           //   companion: unlike the three tables above, the glyph table is not
                           //   regioned per (frame, viewport) -- it is written only when its
                           //   generation moves, so one copy serves every frame and surface.
};
[[vk::push_constant]] gui_pc_t pc;

// The bindless storage-buffer array (set 0, binding 2).  The gui reads FOUR tables through it:
// the frame's clip entries, the style records, the quad records, and the glyph UV table.  Declared
// float4[] because that is the one element type the array can have; integer lanes come back
// through asuint, a reinterpret, not a convert.
[[vk::binding( 2, 0 )]] StructuredBuffer<float4> u_buffers[] : register( t0, space1 );

// Decode an sRGB-encoded color to linear light.  UI colors are authored sRGB; the swapchain is
// an _SRGB format, so the GPU blends linear and re-encodes on store.
float3 srgb_to_linear( float3 c )
{
    float3 lo = c / 12.92;
    float3 hi = pow( ( c + 0.055 ) / 1.055, 2.4 );
    return lerp( hi, lo, step( c, 0.04045 ) );    // c <= 0.04045 selects lo (GLSL mix + cutoff)
}

// The record strides, mirroring gui.h (GUI_QUAD_ROWS / GUI_PRIM_ROWS).  BOTH stages index the
// style buffer -- the vertex stage reads the feather to grow a skirt quad, the fragment reads
// everything -- so the stride lives here rather than once per stage: two copies that disagree
// read two different records and the mismatch shows up as a shape, not as an error.
#define QUAD_ROWS       2u
#define PRIM_ROWS       8u

// The quad's packed INDEX word, mirroring gui.h.  Rule and glyph bit at the bottom, then the
// slot-local clip entry, style record and fx row; the word is exactly full.
#define GUI_QUAD_F_GLYPH       ( 1u << 2 )
#define GUI_QUAD_CLIP_SHIFT    3u
#define GUI_QUAD_CLIP_MASK     0xFu
#define GUI_QUAD_STYLE_SHIFT   7u
#define GUI_QUAD_STYLE_MASK    0x7FFu
#define GUI_QUAD_FX_SHIFT      18u

// The INSTANCE EXTRAS record (gui_fx_t): turn, animation phase, border colour.  It lives in the
// STYLE arena, eight rows to a record, and the quad names it by slot-local ROW index -- so the
// fetch is the style buffer at the slot's base, offset by rows rather than by records.  Row 0 can
// never be one, which is what lets 0 mean "no record": identity turn, zero phase, no border.
float4 fx_record( uint row )
{
    return row ? u_buffers[ pc.prim_buf ][ pc.prim_base * PRIM_ROWS + row ]
               : float4( 0.0, 0.0, 0.0, 0.0 );
}

// A glyph table entry is TWO uints, so two entries share one float4 row: the ID's high bits pick
// the row and its low bit picks the half.  Mirrors gui_glyph_uv_t (gui.h).

uint2 glyph_uv( uint id )
{
    float4 row = u_buffers[ pc.glyph_buf ][ id >> 1u ];
    return ( id & 1u ) ? uint2( asuint( row.z ), asuint( row.w ) )
                       : uint2( asuint( row.x ), asuint( row.y ) );
}

// A pair of unorm16 as two floats over [0,1], x in the LOW half -- the packing gui_uv_pack writes
// (gui.h).  Shared so the one place that knows the encoding is not two places: the vertex stage
// reads uvs and the shape's turn with it, the fragment reads a pattern phase.
float2 unpack_unorm16x2( uint p )
{
    return float2( p & 0xFFFFu, p >> 16u ) / 65535.0;
}

// An RGBA8 word (R in the low byte) as linear-light colour.
float4 unpack_col( uint c )
{
    float4 v = float4( float(   c         & 0xFFu ), float( ( c >>  8 ) & 0xFFu ),
                       float( ( c >> 16 ) & 0xFFu ), float( ( c >> 24 ) & 0xFFu ) ) / 255.0;
    return float4( srgb_to_linear( v.rgb ), v.a );
}
