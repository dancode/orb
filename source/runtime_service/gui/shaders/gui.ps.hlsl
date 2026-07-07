// gui.ps.hlsl -- HLSL twin of gui.frag (the cooked-shader path).
//
// Cooked into bin/shaders/gui.ps.oshd; see gui.vs.hlsl for the cooked-vs-fallback contract.
// Keep the push constant block identical to the vertex stage and to gui_push_t.

struct gui_pc_t
{
    float4x4 mvp;        // column-major pixel-space ortho (Vulkan clip space)
    uint     tex_idx;    // bindless texture slot
    uint     samp_idx;   // bindless sampler slot
    uint     dbg_flat;   // debug: 1 = ignore atlas coverage, output a flat color
    uint     dbg_tint;   // debug: packed RGBA8 batch tint (0 = use vertex color)
    uint     rgba_tex;   // 1 = sample tex_idx as a full RGBA image, not R8 coverage
};
[[vk::push_constant]] gui_pc_t pc;

[[vk::binding( 0, 0 )]] Texture2D    u_textures[] : register( t0, space0 );
[[vk::binding( 1, 0 )]] SamplerState u_samplers[] : register( s0, space0 );

struct ps_in_t
{
    float4 sv_pos : SV_Position;
    float4 color  : COLOR0;
    float2 uv     : TEXCOORD0;
};

// Decode an sRGB-encoded color to linear light.  UI colors are authored in sRGB (the values
// you type as hex / pick in a color picker), but the swapchain is a _SRGB format, so the GPU
// blends in linear space and re-encodes on store.  The vertex color arrives as raw UNORM bytes
// (no automatic decode), so we linearize it here to keep alpha blending physically correct.
float3 srgb_to_linear( float3 c )
{
    float3 lo = c / 12.92;
    float3 hi = pow( ( c + 0.055 ) / 1.055, 2.4 );
    return lerp( hi, lo, step( c, 0.04045 ) );    // c <= 0.04045 selects lo (GLSL mix + cutoff)
}

float4 main( ps_in_t i ) : SV_Target0
{
    // Debug views: bypass the atlas so geometry is visible regardless of glyph coverage.
    //   wireframe -- the LINE pipeline strokes triangle edges; a flat opaque color makes them
    //                show even across text quads (where s.r would otherwise alpha them away).
    //   batch     -- each draw call is pushed a distinct dbg_tint so its geometry reads as one
    //                solid color block; a color change marks a batch split.
    if ( pc.dbg_flat != 0u )
    {
        float3 rgb;
        float  a;
        if ( pc.dbg_tint != 0u )
        {
            rgb = float3( float(   pc.dbg_tint         & 0xFFu ),
                          float( ( pc.dbg_tint >> 8  ) & 0xFFu ),
                          float( ( pc.dbg_tint >> 16 ) & 0xFFu ) ) / 255.0;
            a   = float( ( pc.dbg_tint >> 24 ) & 0xFFu ) / 255.0;
        }
        else
        {
            rgb = i.color.rgb;    // wireframe keeps each window's own color
            a   = 1.0;
        }
        return float4( srgb_to_linear( rgb ), a );
    }

    float4 s = u_textures[ pc.tex_idx ].Sample( u_samplers[ pc.samp_idx ], i.uv );

    // Full-RGBA image path (scene viewport / arbitrary bindless texture): the texel IS the
    // color, with the vertex color acting as a tint.  The texel arrives LINEAR: _SRGB-format
    // textures are hardware-decoded at sample time, and UNORM render targets hold linear data.
    // Only the authored tint color needs the sRGB decode.
    if ( pc.rgba_tex != 0u )
        return float4( s.rgb * srgb_to_linear( i.color.rgb ), s.a * i.color.a );

    // Only RGB is gamma-decoded; alpha is linear coverage.  s.r is the glyph coverage from the
    // R8 atlas (1.0 for the white solid-color pixel, so non-text draws pass through).
    return float4( srgb_to_linear( i.color.rgb ), i.color.a * s.r );
}
