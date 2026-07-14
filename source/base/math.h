/*==============================================================================================

    math.h -- Integer and floating-point math utilities.

    Explicit, type-safe API (no C11 _Generic).
    Naming scheme: math_<type>_<operation>

==============================================================================================*/
#ifndef MATH_H
#define MATH_H

// clang-format off
/*==============================================================================================
    Constants
==============================================================================================*/

#define MATH_PI          3.14159265358979323846f
#define MATH_TAU         6.28318530717958647692f
#define MATH_INV_PI      0.31830988618379067154f
#define MATH_PI_OVER_2   1.57079632679489661923f
#define MATH_PI_OVER_4   0.78539816339744830962f

#define MATH_DEG_TO_RAD  ( MATH_PI / 180.0f )
#define MATH_RAD_TO_DEG  ( 180.0f / MATH_PI )

#define F32_EPSILON      1e-6f

/*==============================================================================================
    Intrinsics
==============================================================================================*/

#if COMPILER_MSVC
    #include <math.h>
    #include <intrin.h>
#else
    #include <math.h>
    #define f32_abs( x )        __builtin_fabsf( x )
    #define f64_abs( x )        __builtin_fabs( x )
    #define f32_sqrt( x )       __builtin_sqrtf( x )
    #define f32_sin( x )        __builtin_sinf( x )
    #define f32_cos( x )        __builtin_cosf( x )
    #define f32_tan( x )        __builtin_tanf( x )
    #define f32_acos( x )       __builtin_acosf( x )
    #define f32_asin( x )       __builtin_asinf( x )
    #define f32_atan( x )       __builtin_atanf( x )
    #define f32_atan2( y, x )   __builtin_atan2f( y, x )
    #define f32_floor( x )      __builtin_floorf( x )
    #define f32_ceil( x )       __builtin_ceilf( x )
    #define f32_round( x )      __builtin_roundf( x )
    #define f32_trunc( x )      __builtin_truncf( x )
    #define f32_mod( x, y )     __builtin_fmodf( x, y )
    #define f32_pow( x, y )     __builtin_powf( x, y )
    #define f32_exp( x )        __builtin_expf( x )
    #define f32_exp2( x )       __builtin_exp2f( x )
    #define f32_log( x )        __builtin_logf( x )
    #define f32_log2( x )       __builtin_log2f( x )
    #define f32_copysign( m, s ) __builtin_copysignf( m, s )
#endif

#if COMPILER_MSVC
ORB_INLINE f32 f32_abs   ( f32 x )        { return fabsf  ( x );    }
ORB_INLINE f64 f64_abs   ( f64 x )        { return fabs   ( x );    }
ORB_INLINE f32 f32_sqrt  ( f32 x )        { return sqrtf  ( x );    }
ORB_INLINE f32 f32_sin   ( f32 x )        { return sinf   ( x );    }
ORB_INLINE f32 f32_cos   ( f32 x )        { return cosf   ( x );    }
ORB_INLINE f32 f32_tan   ( f32 x )        { return tanf   ( x );    }
ORB_INLINE f32 f32_acos  ( f32 x )        { return acosf  ( x );    }
ORB_INLINE f32 f32_asin  ( f32 x )        { return asinf  ( x );    }
ORB_INLINE f32 f32_atan  ( f32 x )        { return atanf  ( x );    }
ORB_INLINE f32 f32_atan2 ( f32 y, f32 x ) { return atan2f ( y, x ); }
ORB_INLINE f32 f32_floor ( f32 x )        { return floorf ( x );    }
ORB_INLINE f32 f32_ceil  ( f32 x )        { return ceilf  ( x );    }
ORB_INLINE f32 f32_round ( f32 x )        { return roundf ( x );    }
ORB_INLINE f32 f32_trunc ( f32 x )        { return truncf ( x );    }
ORB_INLINE f32 f32_mod   ( f32 x, f32 y ) { return fmodf  ( x, y ); }
ORB_INLINE f32 f32_pow   ( f32 x, f32 y ) { return powf   ( x, y ); }
ORB_INLINE f32 f32_exp   ( f32 x )        { return expf   ( x );    }
ORB_INLINE f32 f32_exp2  ( f32 x )        { return exp2f  ( x );    }
ORB_INLINE f32 f32_log   ( f32 x )        { return logf   ( x );    }
ORB_INLINE f32 f32_log2  ( f32 x )        { return log2f  ( x );    }
ORB_INLINE f32 f32_copysign( f32 mag, f32 sign ) { return copysignf( mag, sign ); }
#endif

// Reciprocal square root (1/sqrt).  Scalar today; a fast approximation can drop in behind this.
ORB_INLINE f32 f32_rsqrt( f32 x ) { return 1.0f / f32_sqrt( x ); }

// Fractional part: x - floor(x), always in [0, 1).
ORB_INLINE f32 f32_fract( f32 x ) { return x - f32_floor( x ); }

/*==============================================================================================
    Min / Max
==============================================================================================*/

ORB_INLINE i32 i32_min( i32 a, i32 b ) { return a < b ? a : b; }
ORB_INLINE i32 i32_max( i32 a, i32 b ) { return a > b ? a : b; }

ORB_INLINE i64 i64_min( i64 a, i64 b ) { return a < b ? a : b; }
ORB_INLINE i64 i64_max( i64 a, i64 b ) { return a > b ? a : b; }

ORB_INLINE u32 u32_min( u32 a, u32 b ) { return a < b ? a : b; }
ORB_INLINE u32 u32_max( u32 a, u32 b ) { return a > b ? a : b; }

ORB_INLINE u64 u64_min( u64 a, u64 b ) { return a < b ? a : b; }
ORB_INLINE u64 u64_max( u64 a, u64 b ) { return a > b ? a : b; }

ORB_INLINE f32 f32_min( f32 a, f32 b ) { return a < b ? a : b; }
ORB_INLINE f32 f32_max( f32 a, f32 b ) { return a > b ? a : b; }

ORB_INLINE f64 f64_min( f64 a, f64 b ) { return a < b ? a : b; }
ORB_INLINE f64 f64_max( f64 a, f64 b ) { return a > b ? a : b; }

/*==============================================================================================
    Clamp / Abs
==============================================================================================*/

ORB_INLINE i32 i32_clamp( i32 v, i32 lo, i32 hi ) { return i32_min( i32_max( v, lo ), hi ); }
ORB_INLINE i64 i64_clamp( i64 v, i64 lo, i64 hi ) { return i64_min( i64_max( v, lo ), hi ); }
ORB_INLINE u32 u32_clamp( u32 v, u32 lo, u32 hi ) { return u32_min( u32_max( v, lo ), hi ); }
ORB_INLINE u64 u64_clamp( u64 v, u64 lo, u64 hi ) { return u64_min( u64_max( v, lo ), hi ); }
ORB_INLINE f32 f32_clamp( f32 v, f32 lo, f32 hi ) { return f32_min( f32_max( v, lo ), hi ); }
ORB_INLINE f64 f64_clamp( f64 v, f64 lo, f64 hi ) { return f64_min( f64_max( v, lo ), hi ); }

ORB_INLINE i32 i32_abs( i32 a ) { return a < 0 ? -a : a; }
ORB_INLINE i64 i64_abs( i64 a ) { return a < 0 ? -a : a; }

/*==============================================================================================
    Interpolation
==============================================================================================*/

// Numerically stable linear interpolation: a when t=0, b when t=1.
ORB_INLINE f32
f32_lerp( f32 a, f32 b, f32 t )
{
    return ( 1.0f - t ) * a + t * b;
}

ORB_INLINE f64
f64_lerp( f64 a, f64 b, f64 t )
{
    return ( 1.0 - t ) * a + t * b;
}

// Inverse lerp: returns t such that lerp(a, b, t) == v.
ORB_INLINE f32
f32_unlerp( f32 a, f32 b, f32 v )
{
    return ( v - a ) / ( b - a );
}

ORB_INLINE f32
f32_remap( f32 in_lo, f32 in_hi, f32 out_lo, f32 out_hi, f32 v )
{
    return f32_lerp( out_lo, out_hi, f32_unlerp( in_lo, in_hi, v ) );
}

// Clamp to [0, 1] -- the shader `saturate`.  The common case for factors/weights/colors.
ORB_INLINE f32 f32_saturate( f32 x ) { return f32_clamp( x, 0.0f, 1.0f ); }

// Step: 0 below the edge, 1 at or above it (the GLSL `step`).
ORB_INLINE f32 f32_step( f32 edge, f32 x ) { return x < edge ? 0.0f : 1.0f; }

// Move `current` toward `target` by at most `max_delta` (never overshoots).
ORB_INLINE f32
f32_move_toward( f32 current, f32 target, f32 max_delta )
{
    f32 d = target - current;
    if ( f32_abs( d ) <= max_delta ) return target;
    return current + f32_copysign( max_delta, d );
}

// Triangle wave: bounces v back and forth in [0, length] (period 2*length).
ORB_INLINE f32
f32_ping_pong( f32 v, f32 length )
{
    if ( length <= 0.0f ) return 0.0f;
    f32 t = f32_mod( f32_abs( v ), 2.0f * length );
    return t <= length ? t : 2.0f * length - t;
}

/*==============================================================================================
    Angles  (radians)
==============================================================================================*/

ORB_INLINE f32 f32_deg_to_rad( f32 deg ) { return deg * MATH_DEG_TO_RAD; }
ORB_INLINE f32 f32_rad_to_deg( f32 rad ) { return rad * MATH_RAD_TO_DEG; }

// Wrap an angle into (-PI, PI].
ORB_INLINE f32
f32_wrap_pi( f32 a )
{
    a = f32_mod( a + MATH_PI, MATH_TAU );
    if ( a < 0.0f ) a += MATH_TAU;
    return a - MATH_PI;
}

// Shortest signed delta from `a` to `b`, result in (-PI, PI].
ORB_INLINE f32 f32_angle_diff( f32 a, f32 b ) { return f32_wrap_pi( b - a ); }

// Interpolate along the shortest arc from `a` to `b`.
ORB_INLINE f32 f32_lerp_angle( f32 a, f32 b, f32 t ) { return a + f32_angle_diff( a, b ) * t; }

/*==============================================================================================
    Sign / Compare
==============================================================================================*/

ORB_INLINE i32 i32_sign( i32 x ) { return ( x > 0 ) - ( x < 0 ); }
ORB_INLINE f32 f32_sign( f32 x ) { return ( f32 )( ( x > 0.0f ) - ( x < 0.0f ) ); }

ORB_INLINE b32
f32_nearly_equal( f32 a, f32 b, f32 eps )
{
    return f32_abs( a - b ) <= eps;
}

/*==============================================================================================
    Alignment
==============================================================================================*/

// Align v up to next multiple of align (align must be power of two).
#define math_align_up( v, align )   ( ( ( v ) + ( align ) - 1 ) & ~( ( align ) - 1 ) )

// Align v down to nearest multiple of align (align must be power of two).
#define math_align_down( v, align ) ( ( v ) & ~( ( align ) - 1 ) )

/*==============================================================================================
    Compile-time integer log2 (for constants only)
==============================================================================================*/

#define MATH_LOG2_CONST(n) \
    ((n) <= (1<<0)  ? 0  : (n) <= (1<<1)  ? 1  : (n) <= (1<<2)  ? 2  : \
     (n) <= (1<<3)  ? 3  : (n) <= (1<<4)  ? 4  : (n) <= (1<<5)  ? 5  : \
     (n) <= (1<<6)  ? 6  : (n) <= (1<<7)  ? 7  : (n) <= (1<<8)  ? 8  : \
     (n) <= (1<<9)  ? 9  : (n) <= (1<<10) ? 10 : (n) <= (1<<11) ? 11 : \
     (n) <= (1<<12) ? 12 : (n) <= (1<<13) ? 13 : (n) <= (1<<14) ? 14 : \
     (n) <= (1<<15) ? 15 : (n) <= (1<<16) ? 16 : 31)

/*==============================================================================================
    Vectors, matrices, quaternions live in their own headers (all pulled in by base.h):
        math_vec.h   -- vec2 / vec3 / vec4 / vec2i
        math_mat.h   -- mat3 / mat4  (column-major, right-handed, Vulkan clip)
        math_quat.h  -- quat
==============================================================================================*/

// clang-format on
/*============================================================================================*/
#endif    // MATH_H