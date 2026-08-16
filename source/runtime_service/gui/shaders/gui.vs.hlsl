// gui.vs.hlsl -- HLSL twin of gui.vert (the cooked-shader path).
//
// Cooked by shader_tool (via cook_shaders.bat) into bin/shaders/gui.vs.oshd; gui_render_init
// prefers the cooked pair when both files sit next to the exe and falls back to the embedded
// SPIR-V in pipeline/gui_shader.h (compiled from the GLSL twins) when they are absent.  Keep
// this file, the GLSL, and gui_push_t in gui_render.c in lockstep -- the push constant block
// and vertex inputs must stay byte-identical or the cooked and fallback paths diverge.
//
// The mvp is authored in VULKAN clip space (render_ortho maps top-left to -1,-1 with +y down),
// which is why the GLSL twin compiles without any y flip.  shader_tool bakes -fvk-invert-y
// into every vertex-stage cook (house convention: HLSL sources are D3D +y-up), so this shader
// negates y once to cancel it and match the fallback exactly.

struct gui_pc_t
{
    float4x4 mvp;          // column-major pixel-space ortho (Vulkan clip space)
    uint     samp_point;   // bindless sampler slot: NEAREST, for the coverage model
    uint     samp_image;   // bindless sampler slot: LINEAR, for every model that filters
    uint     dbg_flat;     // debug: 1 = ignore atlas coverage, output a flat color
    uint     dbg_tint;     // debug: packed RGBA8 batch tint (0 = use vertex color)
    float    time;         // effect-band frame clock, wrapped seconds (GUI_FX_TIME_WRAP)
    uint     clip_buf;     // bindless buffer slot of the frame's clip table (fragment-only)
    uint     clip_base;    // the flush's clip-region origin in the table (fragment-only)
    uint     prim_buf;     // bindless buffer slot of the primitive records (fragment-only)
    uint     prim_base;    // this window slot's first record (fragment-only)
};
[[vk::push_constant]] gui_pc_t pc;

// TWO of these attributes are PACKED in memory (gui.h): uv is two unorm16 and color is four
// unorm8.  Neither says so here, and that is the point -- vertex fetch widens normalized formats
// to 32-bit float before the shader sees them.
//
// The effect COORDINATE used to be a third packed attribute, two halves of `|p| - c` computed per
// vertex.  The fragment derives it from its own pixel position and the record now, which is what
// let the quadrant tessellation collapse: this stage no longer knows a shape is involved.
struct vs_in_t
{
    [[vk::location( 0 )]] float2 pos   : POSITION;
    [[vk::location( 1 )]] float2 uv    : TEXCOORD0;
    [[vk::location( 2 )]] float4 color : COLOR0;      // UNORM4 attrib -> normalized float4
    [[vk::location( 3 )]] uint   prim  : TEXCOORD1;   // primitive record index, slot-local
};

// nointerpolation on prim: it is an index into a storage buffer, and interpolating it would name a
// different shape on every pixel.
struct vs_out_t
{
    float4                  sv_pos : SV_Position;
    float4                  color  : COLOR0;
    float2                  uv     : TEXCOORD0;
    nointerpolation uint    prim   : TEXCOORD1;
};

// Decode an sRGB-encoded color to linear light.  UI colors are authored in sRGB (the values you
// type as hex / pick in a color picker), but the swapchain is a _SRGB format, so the GPU blends in
// linear space and re-encodes on store.  The vertex color arrives as raw UNORM bytes (no automatic
// decode), so it has to be linearized for alpha blending to be physically correct.
//
// It happens HERE, not in the fragment, because the quantity is per-vertex constant: decoding it
// per fragment ran three pow() over every pixel of every panel to recompute a number that changes
// four times per quad.  UI is fill-bound -- a docked layout is mostly large flat rects -- so this
// was the most expensive thing in the fragment shader and the least necessary.
//
// The consequence is that a MULTI-COLOR quad now interpolates in linear light rather than in sRGB.
// For every primitive whose four vertices share one color (fills, text, sprites, SDF surfaces) the
// result is bit-identical.  It differs for GUI_CMD_RECT_GRADIENT, which exists precisely to let the
// hardware blend two colors: its midpoint is now the photometric mean rather than the perceptual
// one, and reads lighter.  That is the same rule the alpha blend already follows, so the two are
// consistent for the first time -- but it IS a visible change to authored gradients.
float3 srgb_to_linear( float3 c )
{
    float3 lo = c / 12.92;
    float3 hi = pow( ( c + 0.055 ) / 1.055, 2.4 );
    return lerp( hi, lo, step( c, 0.04045 ) );    // c <= 0.04045 selects lo (GLSL mix + cutoff)
}

vs_out_t main( vs_in_t v )
{
    vs_out_t o;
    o.sv_pos   = mul( pc.mvp, float4( v.pos, 0.0, 1.0 ) );
    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
    // RGB linear, alpha untouched -- alpha is coverage, which is already a linear quantity.
    o.color    = float4( srgb_to_linear( v.color.rgb ), v.color.a );

    // GUI_FX_TILE_U used to scale U here, from the packed effect word.  It scales in the FRAGMENT
    // now, against the record: the multiply is affine and commutes with interpolation, so the two
    // are exactly equivalent.  The stored U is still normalized 0..1 (all UNORM16X2 can hold) and
    // the sampler's REPEAT is still what tiles the atlas stipple row.
    o.uv   = v.uv;
    o.prim = v.prim;
    return o;
}
