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
    uint     prim_buf;     // bindless buffer slot of the prim records
    uint     prim_base;    // this window SLOT's first record (records, not float4s)
    uint     pal_base;     // this FRAME's palette block, in the same buffer past every region.
                           //   Flush-constant where prim_base is not: a palette index resolves
                           //   the same for every window slot (see prim_base_row below)
    uint     quad_buf;     // bindless buffer slot of the quad records (gui_quad_t)
    uint     quad_base;    // the flush's quad-region origin (quads, not float4s)
    uint     glyph_buf;    // bindless buffer slot of the glyph UV table (gui_glyph_uv_t).  No base
                           //   companion: unlike the three tables above, the glyph table is not
                           //   regioned per (frame, viewport) -- it is written only when its
                           //   generation moves, so one copy serves every frame and surface.
    uint     tex_cov;      // bindless texture slot of the R8 coverage atlas
    uint     tex_sdf;      // bindless texture slot of the SDF atlas.  A GLYPH-tagged quad names no
                           //   prim record, so its texture comes from one of these two rather
                           //   than off a record -- picked by the tag's SDF bit.
};
[[vk::push_constant]] gui_pc_t pc;

// The bindless storage-buffer array (set 0, binding 2).  The gui reads FOUR tables through it:
// the frame's clip entries, the prim records, the quad records, and the glyph UV table.  Declared
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
// prim buffer -- the vertex stage reads the feather to grow a skirt quad, the fragment reads
// everything -- so the stride lives here rather than once per stage: two copies that disagree
// read two different records and the mismatch shows up as a shape, not as an error.
#define QUAD_ROWS       1u
#define PRIM_ROWS       8u
#define FX_ROWS         2u

// The quad's packed INDEX word, a TAGGED UNION mirroring gui.h.  The tag is the top two bits and
// the clip entry sits at the bottom of every layout, so clip decodes without reading the tag.
#define GUI_QUAD_TAG_SHIFT     30u
#define GUI_QUAD_TAG_GLYPH     1u
#define GUI_QUAD_TAG_BAND      2u
#define GUI_QUAD_TAG_GSTYLED   3u

#define GUI_QUAD_CLIP_SHIFT    0u
#define GUI_QUAD_CLIP_MASK     0xFu
#define GUI_QUAD_RULE_SHIFT    4u
#define GUI_QUAD_BAND_SHIFT    4u   // the rule field, re-read under the BAND tag
#define GUI_QUAD_PRIM_SHIFT   6u
#define GUI_QUAD_PRIM_MASK    0x7FFu
#define GUI_QUAD_FX_SHIFT      17u
#define GUI_QUAD_FX_MASK       0x1FFFu

// The prim field's PALETTE half (gui.h, GUI_PAL_FIRST).  An index below this names a record in
// the emitting window slot's own arena run; at or above it, a shared record in the frame's palette
// block, which is why one entry can serve every window on every surface.  The index itself is what
// picks the base -- there is no flag to carry and no second field to keep in step.
//
// Every prim fetch in either stage goes through this: the vertex stage's band hole and skirt pad,
// and the fragment's record head.  fx rows deliberately do NOT -- they stay slot-local against
// prim_base, so a palette prim still composes with a per-instance turn, phase and uv rect.
#define GUI_PAL_FIRST          1024u

uint prim_base_row( uint prim )
{
    uint rec = ( prim >= GUI_PAL_FIRST ) ? ( pc.pal_base  + ( prim - GUI_PAL_FIRST ) )
                                         : ( pc.prim_base + prim );
    return rec * PRIM_ROWS;
}

// The prim record's OP bits and the field enum, mirroring gui_prim.h's GUI_OP_* and
// gui_fx_mode_t.  Both stages read the ops -- the fragment to run the cascade, the vertex
// stage to size a band covering -- so this is the one shader-side copy.  Keep it in step with
// gui_prim.h and rebuild.
#define OP_BAND         0x1u
#define OP_CUT          0x2u
#define OP_INSET        0x4u
#define OP_GLOW         0x8u
#define OP_SWELL        0x10u
#define OP_CUT_SHAPE    0x20u
#define OP_SELF         0x40u
#define OP_GRAD         0x80u
#define OP_GRAD_RADIAL  0x100u
#define OP_GRAD_CONIC   0x200u
#define OP_GRAD_ALONG   0x400u
#define OP_GRAD_CELL    0x800u
#define OP_CELL_FILL    0x1000u
#define OP_FRAME        0x2000u
#define OP_TILE_U       0x4000u
#define OP_TEXT_EDGE    0x8000u
#define OP_CHECKER      0x10000u
#define OP_GRID         0x20000u
#define OP_STRIPES      0x40000u
#define OP_REPEAT       0x80000u
#define OP_REPEAT_POLAR 0x100000u
#define OP_PULSE        0x200000u
#define OP_SPIN         0x400000u
#define OP_DASH         0x800000u
#define OP_DITHER       0x1000000u

#define FX_NONE         0u
#define FX_BOX          1u
#define FX_NGON         2u
#define FX_TRI          3u
#define FX_BEZIER       4u
#define FX_SEG          5u
#define FX_ARC          6u
#define FX_PIE          7u
#define FX_TEX          8u

#define GUI_QUAD_SDF_BIT       ( 1u << 4 )
#define GUI_QUAD_GLYPH_SHIFT   5u
#define GUI_QUAD_GLYPH_MASK    0x1FFFu
#define GUI_QUAD_GFX_SHIFT     18u
#define GUI_QUAD_GFX_MASK      0xFFFu
#define GUI_QUAD_GPRIM_SHIFT  18u   // GLYPH_STYLED: a prim record where GLYPH keeps fx bits
#define GUI_QUAD_GPRIM_MASK   0x7FFu

// The INSTANCE EXTRAS record (gui_fx_t): row A is the turn, the animation phase, the border
// colour and the swell amplitude; row B is the texture rect.  It lives in the PRIM arena, four records to a prim slot,
// and the quad names it by slot-local ROW index -- so the fetch is the prim buffer at the slot's
// base, offset by rows rather than by records.  Row 0 can never be one, which is what lets 0 mean
// "no record": identity turn, zero phase, no border, no texture rect.
//
// `sub` selects the row within the record: 0 = the instance lanes, 1 = the uv rect only a textured
// quad asks for, so the second load is paid by the quads that need it.
float4 fx_record( uint row, uint sub )
{
    return row ? u_buffers[ pc.prim_buf ][ pc.prim_base * PRIM_ROWS + row + sub ]
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
