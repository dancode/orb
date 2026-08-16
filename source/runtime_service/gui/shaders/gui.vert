#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    uint samp_point;  // bindless sampler slot: NEAREST, for the coverage model
    uint samp_image;  // bindless sampler slot: LINEAR, for every model that filters
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    float time;      // effect-band frame clock, seconds wrapped to GUI_FX_TIME_WRAP (1024)
    uint clip_buf;   // bindless buffer slot of the frame's clip table (fragment-only)
    uint clip_base;  // the flush's clip-region origin in the table (fragment-only)
    uint prim_buf;   // bindless buffer slot of the primitive records (fragment-only)
    uint prim_base;  // this window slot's first record (fragment-only)
} pc;

// TWO of these attributes are PACKED in memory (gui.h): uv is two unorm16 and color is four
// unorm8.  Neither says so here, and that is the point -- vertex fetch widens normalized formats
// to 32-bit float before the shader sees them.
//
// The effect COORDINATE used to be a third packed attribute, two halves of `|p| - c` computed per
// vertex.  The fragment derives it from its own pixel position and the record now, which is what
// let the quadrant tessellation collapse: this stage no longer knows a shape is involved.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_prim;       // primitive record index, slot-local

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
// flat: this is an index into a storage buffer, and interpolating it would name a different shape
// on every pixel of the primitive.
layout(location = 2) flat out uint v_prim;

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
vec3 srgb_to_linear( vec3 c )
{
    bvec3 cutoff = lessThanEqual( c, vec3( 0.04045 ) );
    vec3  lo     = c / 12.92;
    vec3  hi     = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
    return mix( hi, lo, vec3( cutoff ) );
}

void main()
{
    gl_Position = pc.mvp * vec4( in_pos, 0.0, 1.0 );
    // RGB linear, alpha untouched -- alpha is coverage, which is already a linear quantity.
    v_color    = vec4( srgb_to_linear( in_color.rgb ), in_color.a );

    // GUI_FX_TILE_U used to scale U here, from the packed effect word.  It scales in the FRAGMENT
    // now, against the record: the multiply is affine and commutes with interpolation, so the two
    // are exactly equivalent.  The stored U is still normalized 0..1 (all UNORM16X2 can hold) and
    // the sampler's REPEAT is still what tiles the atlas stipple row.
    v_uv       = in_uv;
    v_prim     = in_prim;
}
