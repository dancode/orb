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
};
[[vk::push_constant]] gui_pc_t pc;

// The bindless storage-buffer array (set 0, binding 2).  The gui reads THREE tables through it:
// the frame's clip entries, the style records, and the quad records.  Declared float4[] because
// that is the one element type the array can have; integer lanes come back through asuint, a
// reinterpret, not a convert.
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
#define QUAD_ROWS       3u
#define PRIM_ROWS       8u

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
