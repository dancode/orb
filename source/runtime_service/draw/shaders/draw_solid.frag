#version 450

// Solid fragment shader.  Colors arrive LINEAR (draw()->rect etc. take linear rgba); the
// _SRGB swapchain format re-encodes to sRGB on store, so no decode happens here.

layout(location = 0) in  vec4 in_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = in_color;
}
