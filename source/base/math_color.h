/*==============================================================================================

    base/math_color.h -- color packing, unpacking and color-space conversion.

    One shared color vocabulary for the engine.  Two representations:

      - u32 packed, byte order R,G,B,A from the low byte up:  (a<<24)|(b<<16)|(g<<8)|r.
        This is R8G8B8A8_UNORM in GPU memory and matches the gui's `abgr` convention exactly,
        so a packed color drops straight into a vertex/push-constant.
      - vec4 float, each channel in [0,1], for math (lerp, tint, lighting) before packing.

    sRGB vs linear: packed 8-bit colors are conventionally sRGB (gamma) encoded; do lighting math
    in linear space and convert at the edges with color_srgb_to_linear / color_linear_to_srgb.

    Naming: color_<operation>.  Depends on math_vec.h.

==============================================================================================*/
#ifndef MATH_COLOR_H
#define MATH_COLOR_H

// clang-format off
/*==============================================================================================
    Pack / unpack  (u32 <-> bytes <-> vec4)
==============================================================================================*/

// Pack 8-bit channels into R8G8B8A8 (R in the low byte).
ORB_INLINE u32 color_rgba8( u32 r, u32 g, u32 b, u32 a ) { return ( a << 24 ) | ( b << 16 ) | ( g << 8 ) | ( r & 0xFFu ); }

ORB_INLINE u32 color_get_r( u32 c ) { return ( c       ) & 0xFFu; }
ORB_INLINE u32 color_get_g( u32 c ) { return ( c >> 8  ) & 0xFFu; }
ORB_INLINE u32 color_get_b( u32 c ) { return ( c >> 16 ) & 0xFFu; }
ORB_INLINE u32 color_get_a( u32 c ) { return ( c >> 24 ) & 0xFFu; }

// Replace just the alpha channel of a packed color.
ORB_INLINE u32 color_with_alpha( u32 c, u32 a ) { return ( c & 0x00FFFFFFu ) | ( ( a & 0xFFu ) << 24 ); }

// Pack a float color (channels clamped to [0,1]) into R8G8B8A8.
ORB_INLINE u32
color_pack( vec4_t c )
{
    u32 r = ( u32 )( f32_saturate( c.r ) * 255.0f + 0.5f );
    u32 g = ( u32 )( f32_saturate( c.g ) * 255.0f + 0.5f );
    u32 b = ( u32 )( f32_saturate( c.b ) * 255.0f + 0.5f );
    u32 a = ( u32 )( f32_saturate( c.a ) * 255.0f + 0.5f );
    return color_rgba8( r, g, b, a );
}

// Unpack R8G8B8A8 to a float color in [0,1].
ORB_INLINE vec4_t
color_unpack( u32 c )
{
    return ( vec4_t ){
        .r = ( f32 )color_get_r( c ) / 255.0f,
        .g = ( f32 )color_get_g( c ) / 255.0f,
        .b = ( f32 )color_get_b( c ) / 255.0f,
        .a = ( f32 )color_get_a( c ) / 255.0f,
    };
}

// Interpolate two packed colors (unpacks, lerps, repacks).
ORB_INLINE u32 color_lerp( u32 a, u32 b, f32 t ) { return color_pack( vec4_lerp( color_unpack( a ), color_unpack( b ), t ) ); }

/*==============================================================================================
    sRGB <-> linear
==============================================================================================*/

// Single channel, exact sRGB transfer curve.
ORB_INLINE f32
f32_srgb_to_linear( f32 c )
{
    return c <= 0.04045f ? c / 12.92f : f32_pow( ( c + 0.055f ) / 1.055f, 2.4f );
}

ORB_INLINE f32
f32_linear_to_srgb( f32 c )
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * f32_pow( c, 1.0f / 2.4f ) - 0.055f;
}

// Whole color; alpha is left linear (alpha is not gamma encoded).
ORB_INLINE vec4_t color_srgb_to_linear( vec4_t c ) { return ( vec4_t ){ .r = f32_srgb_to_linear( c.r ), .g = f32_srgb_to_linear( c.g ), .b = f32_srgb_to_linear( c.b ), .a = c.a }; }
ORB_INLINE vec4_t color_linear_to_srgb( vec4_t c ) { return ( vec4_t ){ .r = f32_linear_to_srgb( c.r ), .g = f32_linear_to_srgb( c.g ), .b = f32_linear_to_srgb( c.b ), .a = c.a }; }

/*==============================================================================================
    HSV <-> RGB   (all channels in [0,1]; hue wraps at 1.0)
==============================================================================================*/

// hsv.x = hue, hsv.y = saturation, hsv.z = value.  Returns rgb in [0,1].
ORB_INLINE vec3_t
color_hsv_to_rgb( vec3_t hsv )
{
    f32 h = hsv.x - f32_floor( hsv.x );         // wrap hue into [0,1)
    f32 s = hsv.y, v = hsv.z;
    f32 i = f32_floor( h * 6.0f );
    f32 f = h * 6.0f - i;
    f32 p = v * ( 1.0f - s );
    f32 q = v * ( 1.0f - s * f );
    f32 t = v * ( 1.0f - s * ( 1.0f - f ) );
    switch ( ( i32 )i % 6 )
    {
        case 0:  return vec3_make( v, t, p );
        case 1:  return vec3_make( q, v, p );
        case 2:  return vec3_make( p, v, t );
        case 3:  return vec3_make( p, q, v );
        case 4:  return vec3_make( t, p, v );
        default: return vec3_make( v, p, q );
    }
}

// Returns ( hue, saturation, value ), each in [0,1].
ORB_INLINE vec3_t
color_rgb_to_hsv( vec3_t rgb )
{
    f32 max = f32_max( rgb.r, f32_max( rgb.g, rgb.b ) );
    f32 min = f32_min( rgb.r, f32_min( rgb.g, rgb.b ) );
    f32 delta = max - min;

    f32 h = 0.0f;
    if ( delta > F32_EPSILON )
    {
        if ( max == rgb.r )      h = ( rgb.g - rgb.b ) / delta + ( rgb.g < rgb.b ? 6.0f : 0.0f );
        else if ( max == rgb.g ) h = ( rgb.b - rgb.r ) / delta + 2.0f;
        else                     h = ( rgb.r - rgb.g ) / delta + 4.0f;
        h /= 6.0f;
    }
    f32 s = max > F32_EPSILON ? delta / max : 0.0f;
    return vec3_make( h, s, max );
}

// clang-format on
/*============================================================================================*/
#endif    // MATH_COLOR_H
