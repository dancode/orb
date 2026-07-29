#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    uint samp_point;  // bindless sampler slot: NEAREST, for the coverage model
    uint samp_image;  // bindless sampler slot: LINEAR, for every model that filters
    uint dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color (wireframe / batch view)
    uint dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    float time;      // effect-band frame clock, seconds wrapped to GUI_FX_TIME_WRAP (1024)
} pc;

// FOUR of these six attributes are PACKED in memory (gui.h): uv is two unorm16, color is four
// unorm8, and the effect coord is two halves.  None of that appears here, and that is the point --
// vertex fetch widens normalized and half formats to 32-bit float before the shader sees them, so
// the declarations below are what they were when every field was full width.  The vertex is 28
// bytes instead of 36 for no shader change at all.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_fx_coord;   // effect coord: |p| - c, shape-local pixels
layout(location = 4) in uint in_fx;         // packed effect word; low nibble 0 = no effect
layout(location = 5) in uint in_tex;        // sampling model (top 4 bits) | bindless slot

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_fx_coord;
// flat: the effect word names the SHAPE, which is constant over it -- interpolating a bit field
// would blend a radius into a mode.  Nothing else in the band needs to travel per fragment.
layout(location = 3) flat out uint v_fx;
// flat for the same reason, and more sharply: this one is a descriptor index.
layout(location = 4) flat out uint v_tex;

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

    // GUI_FX_TILE_U: the stored U is normalized 0..1 (all UNORM16X2 can hold) and the repeat count
    // rides the effect word.  Scaling HERE rather than in the fragment is what keeps it free: the
    // hardware interpolates the scaled value exactly as it would have interpolated a wide U, so a
    // dashed line is still one quad tiling the atlas stipple row under the sampler's REPEAT.
    v_uv       = in_uv;
    if ( ( in_fx & 0xFu ) == 4u )
        v_uv.x *= float( ( in_fx >> 4 ) & 0xFFFFFFu ) * 0.0625;

    v_fx_coord = in_fx_coord;
    v_fx       = in_fx;
    v_tex      = in_tex;
}
