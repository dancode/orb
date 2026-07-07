// draw_solid.ps.hlsl -- HLSL twin of draw_solid.frag (the cooked-shader path).
//
// Colors arrive LINEAR (draw()->rect etc. take linear rgba); the _SRGB swapchain format
// re-encodes to sRGB on store, so no decode happens here.

struct ps_in_t
{
    float4 sv_pos : SV_Position;
    float4 color  : COLOR0;
};

float4 main( ps_in_t i ) : SV_Target0
{
    return i.color;
}
