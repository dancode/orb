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
    #define f32_atan2( y, x )   __builtin_atan2f( y, x )
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
ORB_INLINE f32 f32_atan2 ( f32 y, f32 x ) { return atan2f ( y, x ); }
#endif

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

// clang-format on
/*============================================================================================*/
#endif // MATH_H