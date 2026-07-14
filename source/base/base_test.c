/*==============================================================================================

    base/base_test.c -- Base module tests

    Must be linked against the base static library, which provides the implementations.
    Does not compile into base automatically, a test project msut include this file.

==============================================================================================*/
#include <stdio.h> /* printf for demo output */

#include "orb.h"
#include "base/base.h"
#include "base/test.h"

/*==============================================================================================
    Unity Build Tests
==============================================================================================*/

#include "base/str_test.c"

/*==============================================================================================
    char: classification
==============================================================================================*/

static void
test_char_classify( void )
{
    test_true( char_is_alpha( 'A' ) );
    test_true( char_is_alpha( 'z' ) );
    test_true( char_is_alpha( 'm' ) );
    test_false( char_is_alpha( '0' ) );
    test_false( char_is_alpha( '!' ) );
    test_false( char_is_alpha( ' ' ) );

    test_true( char_is_digit( '0' ) );
    test_true( char_is_digit( '9' ) );
    test_false( char_is_digit( 'a' ) );
    test_false( char_is_digit( ' ' ) );

    test_true( char_is_hex( '0' ) );
    test_true( char_is_hex( '9' ) );
    test_true( char_is_hex( 'a' ) );
    test_true( char_is_hex( 'F' ) );
    test_false( char_is_hex( 'g' ) );
    test_false( char_is_hex( 'x' ) );

    test_true( char_is_alnum( 'a' ) );
    test_true( char_is_alnum( '5' ) );
    test_false( char_is_alnum( '!' ) );

    test_true( char_is_upper( 'A' ) );
    test_true( char_is_upper( 'Z' ) );
    test_false( char_is_upper( 'a' ) );
    test_false( char_is_upper( '1' ) );

    test_true( char_is_lower( 'a' ) );
    test_true( char_is_lower( 'z' ) );
    test_false( char_is_lower( 'A' ) );

    test_true( char_is_space( ' ' ) );
    test_true( char_is_space( '\t' ) );
    test_true( char_is_space( '\n' ) );
    test_true( char_is_space( '\r' ) );
    test_false( char_is_space( 'a' ) );
    test_false( char_is_space( '0' ) );

    test_true( char_is_print( 'a' ) );
    test_true( char_is_print( '!' ) );
    test_true( char_is_print( ' ' ) );
    test_false( char_is_print( '\0' ) );
    test_false( char_is_print( '\n' ) );

    test_true( char_is_ctrl( '\0' ) );
    test_true( char_is_ctrl( '\t' ) );
    test_true( char_is_ctrl( 0x7F ) );
    test_false( char_is_ctrl( 'a' ) );
    test_false( char_is_ctrl( ' ' ) );
}

/*==============================================================================================
    char: conversion
==============================================================================================*/

static void
test_char_convert( void )
{
    test_equal( 'A', char_to_upper( 'a' ) );
    test_equal( 'Z', char_to_upper( 'Z' ) );    // already upper: no-op
    test_equal( '5', char_to_upper( '5' ) );    // non-letter: no-op

    test_equal( 'a', char_to_lower( 'A' ) );
    test_equal( 'z', char_to_lower( 'z' ) );    // already lower: no-op
    test_equal( '5', char_to_lower( '5' ) );    // non-letter: no-op

    test_equal( 0, char_digit_value( '0' ) );
    test_equal( 9, char_digit_value( '9' ) );
    test_equal( 5, char_digit_value( '5' ) );
    test_equal( -1, char_digit_value( 'a' ) );
    test_equal( -1, char_digit_value( ' ' ) );

    test_equal( 0, char_hex_value( '0' ) );
    test_equal( 9, char_hex_value( '9' ) );
    test_equal( 10, char_hex_value( 'a' ) );
    test_equal( 10, char_hex_value( 'A' ) );
    test_equal( 15, char_hex_value( 'f' ) );
    test_equal( 15, char_hex_value( 'F' ) );
    test_equal( -1, char_hex_value( 'g' ) );

    test_equal( '0', char_hex_digit( 0 ) );
    test_equal( '9', char_hex_digit( 9 ) );
    test_equal( 'a', char_hex_digit( 10 ) );
    test_equal( 'f', char_hex_digit( 15 ) );
}

/*==============================================================================================
    mem: copy and move
==============================================================================================*/

static void
test_mem_copy_move( void )
{
    u8 src[ 8 ] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u8 dst[ 8 ] = { 0 };

    mem_copy( dst, src, 8 );
    test_true( mem_equal( src, dst, 8 ) );

    // Partial copy leaves rest untouched
    u8 dst2[ 8 ] = { 0 };
    mem_copy( dst2, src, 4 );
    test_true( mem_equal( src, dst2, 4 ) );
    test_equal( 0, dst2[ 4 ] );

    // Overlapping move: shift data right by 4 within the same buffer
    u8 buf[ 16 ] = { 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
    mem_move( buf + 4, buf, 8 );
    test_equal( 1, buf[ 4 ] );
    test_equal( 8, buf[ 11 ] );
}

/*==============================================================================================
    mem: set and zero
==============================================================================================*/

static void
test_mem_set_zero( void )
{
    u8 buf[ 8 ];

    mem_set( buf, 0xAB, 8 );
    for ( i32 i = 0; i < 8; i++ ) test_equal( 0xAB, buf[ i ] );

    mem_zero( buf, 8 );
    for ( i32 i = 0; i < 8; i++ ) test_equal( 0, buf[ i ] );

    // mem_zero_struct
    typedef struct
    {
        i32 x, y;
        f32 z;
    } zero_struct_t;

    zero_struct_t v = { 1, 2, 3.0f };
    mem_zero_struct( &v );
    test_equal( 0, v.x );
    test_equal( 0, v.y );
}

/*==============================================================================================
    mem: compare
==============================================================================================*/

static void
test_mem_compare( void )
{
    u8 a[ 4 ] = { 1, 2, 3, 4 };
    u8 b[ 4 ] = { 1, 2, 3, 4 };
    u8 c[ 4 ] = { 1, 2, 3, 5 };

    test_true( mem_equal( a, b, 4 ) );
    test_false( mem_equal( a, c, 4 ) );
    test_true( mem_equal( a, b, 0 ) );    // zero bytes: always equal

    test_equal( 0, mem_compare( a, b, 4 ) );
    test_true( mem_compare( a, c, 4 ) < 0 );
    test_true( mem_compare( c, a, 4 ) > 0 );
}

/*==============================================================================================
    mem: swap and reverse
==============================================================================================*/

static void
test_mem_swap_reverse( void )
{
    u8 a[ 4 ] = { 1, 2, 3, 4 };
    u8 b[ 4 ] = { 5, 6, 7, 8 };

    mem_swap( a, b, 4 );
    test_equal( 5, a[ 0 ] );
    test_equal( 8, a[ 3 ] );
    test_equal( 1, b[ 0 ] );
    test_equal( 4, b[ 3 ] );

    u8 buf[ 5 ] = { 1, 2, 3, 4, 5 };
    mem_reverse( buf, 5 );
    test_equal( 5, buf[ 0 ] );
    test_equal( 3, buf[ 2 ] );
    test_equal( 1, buf[ 4 ] );

    // mem_swap_t
    i32 x = 10, y = 20;
    mem_swap_t( x, y, i32 );
    test_equal( 20, x );
    test_equal( 10, y );
}

/*==============================================================================================
    mem: alignment
==============================================================================================*/

static void
test_mem_align( void )
{
    test_equal( 8, mem_align_size( 5, 8 ) );
    test_equal( 8, mem_align_size( 8, 8 ) );    // already aligned
    test_equal( 16, mem_align_size( 9, 8 ) );
    test_equal( 32, mem_align_size( 17, 16 ) );
    test_equal( 16, mem_align_size( 16, 16 ) );    // already aligned

    // mem_align_ptr: verify resulting pointer is aligned
    u8    raw[ 64 ];
    void* p = mem_align_ptr( raw + 1, 16 );
    test_equal( 0, ( usize )p % 16 );
    p = mem_align_ptr( raw, 8 );
    test_equal( 0, ( usize )p % 8 );
    p = mem_align_ptr( raw + 3, 4 );
    test_equal( 0, ( usize )p % 4 );
}

/*==============================================================================================
    math: min, max, clamp
==============================================================================================*/

static void
test_math_minmax_clamp( void )
{
    test_equal( 3, i32_min( 3, 5 ) );
    test_equal( 3, i32_min( 5, 3 ) );
    test_equal( 5, i32_max( 3, 5 ) );
    test_equal( 5, i32_max( 5, 3 ) );
    test_equal( -5, i32_min( -5, 3 ) );
    test_equal( 3, i32_max( -5, 3 ) );

    test_equal( 3u, u32_min( 3u, 5u ) );
    test_equal( 5u, u32_max( 3u, 5u ) );

    test_equal( 5, i32_clamp( 5, 0, 10 ) );
    test_equal( 0, i32_clamp( -1, 0, 10 ) );
    test_equal( 10, i32_clamp( 20, 0, 10 ) );
    test_equal( 0, i32_clamp( 0, 0, 10 ) );
    test_equal( 10, i32_clamp( 10, 0, 10 ) );

    test_equal( 5u, u32_clamp( 5u, 0u, 10u ) );
    test_equal( 0u, u32_clamp( 0u, 0u, 10u ) );
    test_equal( 10u, u32_clamp( 99u, 0u, 10u ) );
}

/*==============================================================================================
    math: abs
==============================================================================================*/

static void
test_math_abs( void )
{
    test_equal( 5, i32_abs( 5 ) );
    test_equal( 5, i32_abs( -5 ) );
    test_equal( 0, i32_abs( 0 ) );

    test_equal( 5, i64_abs( -5LL ) );
    test_equal( 0, i64_abs( 0LL ) );

    test_true( f32_abs( -3.14f ) > 0.0f );
    test_true( f32_nearly_equal( f32_abs( -3.14f ), 3.14f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_abs( 3.14f ), 3.14f, F32_EPSILON ) );
}

/*==============================================================================================
    math: lerp, unlerp, remap
==============================================================================================*/

static void
test_math_lerp( void )
{
    test_true( f32_nearly_equal( f32_lerp( 0.0f, 10.0f, 0.0f ), 0.0f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_lerp( 0.0f, 10.0f, 0.5f ), 5.0f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_lerp( 0.0f, 10.0f, 1.0f ), 10.0f, F32_EPSILON ) );

    test_true( f32_nearly_equal( f32_unlerp( 0.0f, 10.0f, 0.0f ), 0.0f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_unlerp( 0.0f, 10.0f, 5.0f ), 0.5f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_unlerp( 0.0f, 10.0f, 10.0f ), 1.0f, F32_EPSILON ) );

    // remap: 5 in [0,10] maps to 50 in [0,100]
    test_true( f32_nearly_equal( f32_remap( 0.0f, 10.0f, 0.0f, 100.0f, 5.0f ), 50.0f, 1e-4f ) );
}

/*==============================================================================================
    math: sign and align
==============================================================================================*/

static void
test_math_sign_align( void )
{
    test_equal( 1, i32_sign( 5 ) );
    test_equal( -1, i32_sign( -5 ) );
    test_equal( 0, i32_sign( 0 ) );

    test_true( f32_nearly_equal( f32_sign( 3.14f ), 1.0f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_sign( -1.0f ), -1.0f, F32_EPSILON ) );
    test_true( f32_nearly_equal( f32_sign( 0.0f ), 0.0f, F32_EPSILON ) );

    test_equal( 8, math_align_up( 5, 8 ) );
    test_equal( 8, math_align_up( 8, 8 ) );    // already aligned
    test_equal( 16, math_align_up( 9, 8 ) );
    test_equal( 8, math_align_down( 13, 8 ) );
    test_equal( 8, math_align_down( 8, 8 ) );    // already aligned
    test_equal( 16, math_align_down( 23, 16 ) );
}

/*==============================================================================================
    math: rng
==============================================================================================*/

static void
test_math_rng( void )
{
    // determinism: same seed reproduces the sequence
    rng_t a, b;
    rng_seed( &a, 12345 );
    rng_seed( &b, 12345 );
    for ( i32 i = 0; i < 16; ++i ) test_equal( rng_u32( &a ), rng_u32( &b ) );

    // different seeds and different streams diverge
    rng_t c, d;
    rng_seed( &c, 12346 );
    rng_seed_stream( &d, 12345, 7 );
    test_true( rng_u32( &a ) != rng_u32( &c ) || rng_u32( &a ) != rng_u32( &c ) );
    test_true( rng_u32( &b ) != rng_u32( &d ) || rng_u32( &b ) != rng_u32( &d ) );

    // bounded draws stay in range; bound 0 is safe
    rng_seed( &a, 999 );
    for ( i32 i = 0; i < 1000; ++i )
    {
        test_true( rng_below( &a, 6 ) < 6 );
        i32 v = rng_range_i32( &a, -3, 3 );
        test_true( v >= -3 && v <= 3 );
    }
    test_equal( 0u, rng_below( &a, 0 ) );

    // floats stay in [0, 1); range respects bounds
    for ( i32 i = 0; i < 1000; ++i )
    {
        f32 f = rng_f32( &a );
        test_true( f >= 0.0f && f < 1.0f );
        f64 g = rng_f64( &a );
        test_true( g >= 0.0 && g < 1.0 );
        f32 h = rng_range_f32( &a, 5.0f, 6.0f );
        test_true( h >= 5.0f && h < 6.0f );
    }

    // chance extremes
    test_false( rng_chance( &a, 0.0f ) );
    test_true( rng_chance( &a, 1.0f ) );

    // sign is only ever -1 or +1
    for ( i32 i = 0; i < 32; ++i )
    {
        i32 s = rng_sign( &a );
        test_true( s == -1 || s == 1 );
    }

    // unit vectors have length ~1
    f32 x, y, z;
    rng_unit2( &a, &x, &y );
    test_true( f32_nearly_equal( x * x + y * y, 1.0f, 1e-4f ) );
    rng_unit3( &a, &x, &y, &z );
    test_true( f32_nearly_equal( x * x + y * y + z * z, 1.0f, 1e-4f ) );
    rng_in_disk( &a, &x, &y );
    test_true( x * x + y * y <= 1.0f );
    rng_in_sphere( &a, &x, &y, &z );
    test_true( x * x + y * y + z * z <= 1.0f );

    // gaussian: sample mean of 4096 draws is near 0
    f32 sum = 0.0f;
    for ( i32 i = 0; i < 4096; ++i ) sum += rng_gauss_f32( &a );
    test_true( f32_abs( sum / 4096.0f ) < 0.1f );

    // shuffle preserves the element multiset
    i32 items[ 8 ] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    rng_shuffle( &a, items, 8, sizeof( i32 ) );
    i32 mask = 0;
    for ( i32 i = 0; i < 8; ++i ) mask |= 1 << items[ i ];
    test_equal( 0xFF, mask );
    rng_shuffle( &a, items, 0, sizeof( i32 ) );    // count 0 is safe

    // weighted pick never lands on a zero-weight bucket
    f32 weights[ 4 ] = { 0.0f, 1.0f, 0.0f, 3.0f };
    for ( i32 i = 0; i < 256; ++i )
    {
        u32 pick = rng_weighted( &a, weights, 4 );
        test_true( pick == 1 || pick == 3 );
    }
    f32 zero_weights[ 2 ] = { 0.0f, 0.0f };
    test_equal( 0u, rng_weighted( &a, zero_weights, 2 ) );

    // scramble: stateless, deterministic, distinct for adjacent inputs
    test_equal( rng_scramble_u64( 42 ), rng_scramble_u64( 42 ) );
    test_true( rng_scramble_u64( 1 ) != rng_scramble_u64( 2 ) );
}

/*==============================================================================================
    math: vectors
==============================================================================================*/

#define test_near( a, b )   test_true( f32_nearly_equal( ( a ), ( b ), 1e-4f ) )

static void
test_math_vec( void )
{
    // layout: field / sub-vector / index aliases share storage; vec4 is 16-aligned
    test_equal( 16, ORB_ALIGNOF( vec4_t ) );
    vec4_t v = vec4_make( 1, 2, 3, 4 );
    test_near( v.e[ 2 ], 3.0f );
    test_near( v.xyz.y, 2.0f );
    test_near( v.zw.x, 3.0f );

    // vec2
    vec2_t a2 = vec2_make( 3, 4 );
    test_near( vec2_len( a2 ), 5.0f );
    test_near( vec2_dot( vec2_make( 1, 0 ), vec2_make( 0, 1 ) ), 0.0f );
    test_near( vec2_cross( vec2_make( 1, 0 ), vec2_make( 0, 1 ) ), 1.0f );
    vec2_t n2 = vec2_normalize( a2 );
    test_near( vec2_len( n2 ), 1.0f );
    test_near( vec2_len( vec2_normalize( vec2_zero() ) ), 0.0f );    // degenerate -> zero

    // vec3
    vec3_t x = vec3_make( 1, 0, 0 ), y = vec3_make( 0, 1, 0 );
    vec3_t z = vec3_cross( x, y );
    test_near( z.z, 1.0f );                                          // x cross y = z (right-handed)
    test_near( vec3_dot( x, y ), 0.0f );
    test_near( vec3_len( vec3_make( 2, 3, 6 ) ), 7.0f );
    vec3_t lp = vec3_lerp( vec3_zero(), vec3_make( 10, 20, 30 ), 0.5f );
    test_near( lp.x, 5.0f ); test_near( lp.y, 10.0f ); test_near( lp.z, 15.0f );

    // reflect off the +Y plane inverts only y
    vec3_t r = vec3_reflect( vec3_make( 1, -1, 0 ), vec3_make( 0, 1, 0 ) );
    test_near( r.x, 1.0f ); test_near( r.y, 1.0f );

    // vec4 + widen/narrow round-trip
    test_near( vec4_dot( vec4_make( 1, 2, 3, 4 ), vec4_make( 1, 1, 1, 1 ) ), 10.0f );
    test_near( vec3_to_vec4( vec3_make( 7, 8, 9 ), 1.0f ).w, 1.0f );
    test_near( vec4_to_vec3( v ).z, 3.0f );

    // vec2i
    test_true( vec2i_eq( vec2i_add( vec2i_make( 1, 2 ), vec2i_make( 3, 4 ) ), vec2i_make( 4, 6 ) ) );
}

/*==============================================================================================
    math: matrices
==============================================================================================*/

static void
test_math_mat( void )
{
    test_equal( 16, ORB_ALIGNOF( mat4_t ) );

    // identity is the multiplicative unit
    mat4_t id = mat4_identity();
    mat4_t t  = mat4_translation( vec3_make( 5, 6, 7 ) );
    mat4_t ti = mat4_mul( id, t );
    for ( i32 i = 0; i < 16; i++ ) test_near( ti.m[ i ], t.m[ i ] );

    // translation moves a point but not a direction
    test_near( mat4_transform_point( t, vec3_make( 1, 1, 1 ) ).x, 6.0f );
    test_near( mat4_transform_dir( t, vec3_make( 1, 1, 1 ) ).x, 1.0f );

    // compose reads right-to-left: scale-then-translate
    mat4_t s  = mat4_scaling( vec3_make( 2, 2, 2 ) );
    mat4_t ts = mat4_mul( t, s );
    vec3_t p  = mat4_transform_point( ts, vec3_make( 1, 0, 0 ) );
    test_near( p.x, 7.0f );                                          // 1*2 + 5
    test_near( p.y, 6.0f );

    // inverse: M * M^-1 == I  (use a non-trivial rotation+translation)
    mat4_t r   = mat4_rotation_axis( vec3_make( 0, 1, 0 ), MATH_PI_OVER_4 );
    mat4_t trm = mat4_mul( t, r );
    mat4_t inv = mat4_inverse( trm );
    mat4_t ii  = mat4_mul( trm, inv );
    for ( i32 i = 0; i < 16; i++ ) test_near( ii.m[ i ], id.m[ i ] );

    // transpose is an involution
    mat4_t tt = mat4_transpose( mat4_transpose( trm ) );
    for ( i32 i = 0; i < 16; i++ ) test_near( tt.m[ i ], trm.m[ i ] );

    // singular matrix -> identity fallback
    mat4_t zero = ( mat4_t ){ 0 };
    mat4_t zinv = mat4_inverse( zero );
    test_near( zinv.m[ 0 ], 1.0f );

    // ortho_2d maps top-left pixel origin to NDC (-1,-1), bottom-right to (+1,+1)
    mat4_t o  = mat4_ortho_2d( 800, 600 );
    vec3_t tl = mat4_transform_point( o, vec3_make( 0, 0, 0 ) );
    vec3_t br = mat4_transform_point( o, vec3_make( 800, 600, 0 ) );
    test_near( tl.x, -1.0f ); test_near( tl.y, -1.0f );
    test_near( br.x,  1.0f ); test_near( br.y,  1.0f );

    // perspective: points on the near plane land at NDC z=0 after the w-divide
    mat4_t proj = mat4_perspective( MATH_PI_OVER_2, 1.0f, 0.5f, 100.0f );
    vec4_t cn   = mat4_mul_vec4( proj, vec4_make( 0, 0, -0.5f, 1 ) );
    test_near( cn.z / cn.w, 0.0f );
    vec4_t cf   = mat4_mul_vec4( proj, vec4_make( 0, 0, -100.0f, 1 ) );
    test_near( cf.z / cf.w, 1.0f );

    // look_at: eye down +Z looking at origin sees the origin straight ahead (view z < 0)
    mat4_t view = mat4_look_at( vec3_make( 0, 0, 10 ), vec3_zero(), vec3_make( 0, 1, 0 ) );
    vec3_t vo   = mat4_transform_point( view, vec3_zero() );
    test_near( vo.z, -10.0f );
}

/*==============================================================================================
    math: quaternions
==============================================================================================*/

static void
test_math_quat( void )
{
    test_equal( 16, ORB_ALIGNOF( quat_t ) );

    // identity rotates nothing
    vec3_t v = vec3_make( 1, 2, 3 );
    vec3_t r = quat_rotate_vec3( quat_identity(), v );
    test_near( r.x, 1.0f ); test_near( r.y, 2.0f ); test_near( r.z, 3.0f );

    // 90deg about +Z sends +X to +Y
    quat_t qz = quat_from_axis_angle( vec3_make( 0, 0, 1 ), MATH_PI_OVER_2 );
    vec3_t rx = quat_rotate_vec3( qz, vec3_make( 1, 0, 0 ) );
    test_near( rx.x, 0.0f ); test_near( rx.y, 1.0f );

    // quat_to_mat4 agrees with quat_rotate_vec3
    mat4_t m  = quat_to_mat4( qz );
    vec3_t mx = mat4_transform_dir( m, vec3_make( 1, 0, 0 ) );
    test_near( mx.x, 0.0f ); test_near( mx.y, 1.0f );

    // compose: q then its conjugate is identity
    quat_t qc = quat_mul( qz, quat_conjugate( qz ) );
    test_near( qc.w, 1.0f ); test_near( qc.x, 0.0f );

    // from_euler matches an equivalent axis-angle (yaw about Y)
    quat_t qe = quat_from_euler( 0.0f, MATH_PI_OVER_2, 0.0f );
    quat_t qy = quat_from_axis_angle( vec3_make( 0, 1, 0 ), MATH_PI_OVER_2 );
    test_near( f32_abs( quat_dot( qe, qy ) ), 1.0f );               // same orientation

    // slerp endpoints are exact; midpoint stays unit
    quat_t s0 = quat_slerp( quat_identity(), qz, 0.0f );
    quat_t s1 = quat_slerp( quat_identity(), qz, 1.0f );
    quat_t sh = quat_slerp( quat_identity(), qz, 0.5f );
    test_near( s0.w, 1.0f );
    test_near( f32_abs( quat_dot( s1, qz ) ), 1.0f );
    test_near( quat_len( sh ), 1.0f );

    // mat4_trs: scale, rotate, translate combined
    mat4_t trs = mat4_trs( vec3_make( 10, 0, 0 ), qz, vec3_make( 2, 2, 2 ) );
    vec3_t tp  = mat4_transform_point( trs, vec3_make( 1, 0, 0 ) );
    test_near( tp.x, 10.0f );                                        // (1*2) rotated to +Y, +10 x
    test_near( tp.y, 2.0f );
}

/*==============================================================================================
    math: scalar helpers (rounding, transcendentals, saturate, angles)
==============================================================================================*/

static void
test_math_scalar( void )
{
    // rounding / fractional
    test_near( f32_floor( 2.7f ), 2.0f );
    test_near( f32_ceil( 2.1f ), 3.0f );
    test_near( f32_round( 2.5f ), 3.0f );
    test_near( f32_trunc( -2.7f ), -2.0f );
    test_near( f32_fract( 2.25f ), 0.25f );
    test_near( f32_mod( 7.0f, 3.0f ), 1.0f );

    // transcendentals + rsqrt
    test_near( f32_pow( 2.0f, 10.0f ), 1024.0f );
    test_near( f32_exp( 0.0f ), 1.0f );
    test_near( f32_log( 1.0f ), 0.0f );
    test_near( f32_log2( 8.0f ), 3.0f );
    test_near( f32_rsqrt( 4.0f ), 0.5f );
    test_near( f32_copysign( 3.0f, -1.0f ), -3.0f );

    // saturate / step / move_toward / ping_pong
    test_near( f32_saturate( 2.0f ), 1.0f );
    test_near( f32_saturate( -1.0f ), 0.0f );
    test_near( f32_saturate( 0.5f ), 0.5f );
    test_near( f32_step( 1.0f, 0.5f ), 0.0f );
    test_near( f32_step( 1.0f, 1.5f ), 1.0f );
    test_near( f32_move_toward( 0.0f, 10.0f, 3.0f ), 3.0f );
    test_near( f32_move_toward( 0.0f, 10.0f, 100.0f ), 10.0f );
    test_near( f32_ping_pong( 0.5f, 1.0f ), 0.5f );
    test_near( f32_ping_pong( 1.5f, 1.0f ), 0.5f );
    test_near( f32_ping_pong( 2.0f, 1.0f ), 0.0f );

    // angles
    test_near( f32_deg_to_rad( 180.0f ), MATH_PI );
    test_near( f32_rad_to_deg( MATH_PI ), 180.0f );
    test_near( f32_wrap_pi( MATH_TAU ), 0.0f );
    test_near( f32_abs( f32_wrap_pi( MATH_PI * 3.0f ) ), MATH_PI );  // 3PI == PI == -PI (range boundary)
    test_near( f32_angle_diff( 0.1f, 0.3f ), 0.2f );
    // shortest path across the +/-PI seam: from just below PI to just above -PI is a small step
    test_true( f32_abs( f32_angle_diff( MATH_PI - 0.1f, -MATH_PI + 0.1f ) ) < 0.5f );
}

/*==============================================================================================
    math: easing / smoothing
==============================================================================================*/

static void
test_math_ease( void )
{
    // smoothstep: clamps, symmetric, midpoint at 0.5
    test_near( f32_smoothstep( 0.0f, 1.0f, -1.0f ), 0.0f );
    test_near( f32_smoothstep( 0.0f, 1.0f, 2.0f ), 1.0f );
    test_near( f32_smoothstep( 0.0f, 1.0f, 0.5f ), 0.5f );
    test_near( f32_smoothstep01( 0.0f ), 0.0f );
    test_near( f32_smoothstep01( 1.0f ), 1.0f );
    test_near( f32_smootherstep01( 0.5f ), 0.5f );

    // damp: moves toward the target without overshoot, frame-rate independent
    f32 d = f32_damp( 0.0f, 10.0f, 5.0f, 0.1f );
    test_true( d > 0.0f && d < 10.0f );

    // easing curves anchor at the endpoints
    test_near( f32_ease_in_cubic( 0.0f ), 0.0f );  test_near( f32_ease_in_cubic( 1.0f ), 1.0f );
    test_near( f32_ease_out_quad( 0.0f ), 0.0f );  test_near( f32_ease_out_quad( 1.0f ), 1.0f );
    test_near( f32_ease_inout_sine( 0.0f ), 0.0f ); test_near( f32_ease_inout_sine( 1.0f ), 1.0f );
    test_near( f32_ease_inout_sine( 0.5f ), 0.5f );
    test_near( f32_ease_out_bounce( 1.0f ), 1.0f );
    test_near( f32_ease_out_elastic( 1.0f ), 1.0f );
    test_near( f32_ease_in_expo( 0.0f ), 0.0f );   test_near( f32_ease_in_expo( 1.0f ), 1.0f );
}

/*==============================================================================================
    math: vector / matrix / quat additions
==============================================================================================*/

static void
test_math_extras( void )
{
    // vec3 project / reject split a vector into parallel + perpendicular parts
    vec3_t a  = vec3_make( 2, 3, 0 );
    vec3_t b  = vec3_make( 1, 0, 0 );
    vec3_t pr = vec3_project( a, b );
    vec3_t rj = vec3_reject( a, b );
    test_near( pr.x, 2.0f ); test_near( pr.y, 0.0f );
    test_near( rj.x, 0.0f ); test_near( rj.y, 3.0f );

    // angle between orthogonal axes is 90 degrees
    test_near( vec3_angle_between( vec3_make( 1, 0, 0 ), vec3_make( 0, 1, 0 ) ), MATH_PI_OVER_2 );

    // move_toward / clamp_length respect the cap
    vec3_t mt = vec3_move_toward( vec3_zero(), vec3_make( 10, 0, 0 ), 3.0f );
    test_near( mt.x, 3.0f );
    vec3_t cl = vec3_clamp_length( vec3_make( 10, 0, 0 ), 4.0f );
    test_near( vec3_len( cl ), 4.0f );

    test_true( vec3_nearly_equal( vec3_make( 1, 2, 3 ), vec3_make( 1, 2, 3 ), 1e-5f ) );

    // integer vectors
    test_true( vec3i_eq( vec3i_add( vec3i_make( 1, 2, 3 ), vec3i_make( 4, 5, 6 ) ), vec3i_make( 5, 7, 9 ) ) );
    test_true( vec4i_eq( vec4i_sub( vec4i_make( 5, 5, 5, 5 ), vec4i_make( 1, 2, 3, 4 ) ), vec4i_make( 4, 3, 2, 1 ) ) );

    // rigid inverse: view = translate * rotate, inverse composes back to identity
    mat4_t t  = mat4_translation( vec3_make( 3, -2, 5 ) );
    mat4_t r  = mat4_rotation_axis( vec3_make( 0, 1, 0 ), MATH_PI_OVER_4 );
    mat4_t tr = mat4_mul( t, r );
    mat4_t ri = mat4_inverse_rigid( tr );
    mat4_t i1 = mat4_mul( tr, ri );
    mat4_t id = mat4_identity();
    for ( i32 i = 0; i < 16; i++ ) test_near( i1.m[ i ], id.m[ i ] );

    // affine inverse: with non-uniform scale too
    mat4_t s  = mat4_scaling( vec3_make( 2, 3, 4 ) );
    mat4_t trs = mat4_mul( tr, s );
    mat4_t ai = mat4_inverse_affine( trs );
    mat4_t i2 = mat4_mul( trs, ai );
    for ( i32 i = 0; i < 16; i++ ) test_near( i2.m[ i ], id.m[ i ] );

    // mat3 inverse round-trip; normal matrix of a pure rotation equals the 3x3 itself
    mat3_t m3  = mat3_from_mat4( r );
    mat3_t m3i = mat3_inverse( m3 );
    mat3_t m3p = mat3_mul( m3, m3i );
    mat3_t m3id = mat3_identity();
    for ( i32 i = 0; i < 9; i++ ) test_near( m3p.m[ i ], m3id.m[ i ] );
    mat3_t nm = mat3_normal_matrix( r );
    for ( i32 i = 0; i < 9; i++ ) test_near( nm.m[ i ], m3.m[ i ] );

    // quat: look_rotation, from_to, angle, euler round-trip, from_mat4
    quat_t lr = quat_look_rotation( vec3_make( 0, 0, -1 ), vec3_make( 0, 1, 0 ) );
    test_near( f32_abs( quat_dot( lr, quat_identity() ) ), 1.0f );   // facing -Z is identity
    vec3_t fwd = quat_rotate_vec3( quat_look_rotation( vec3_make( 1, 0, 0 ), vec3_make( 0, 1, 0 ) ), vec3_make( 0, 0, -1 ) );
    test_near( fwd.x, 1.0f );                                        // -Z rotated to +X

    quat_t ft = quat_from_to( vec3_make( 1, 0, 0 ), vec3_make( 0, 1, 0 ) );
    vec3_t ftv = quat_rotate_vec3( ft, vec3_make( 1, 0, 0 ) );
    test_near( ftv.y, 1.0f );

    test_near( quat_angle( quat_from_axis_angle( vec3_make( 0, 1, 0 ), MATH_PI_OVER_2 ) ), MATH_PI_OVER_2 );

    quat_t qe = quat_from_euler( 0.3f, 0.5f, -0.2f );
    vec3_t ee = quat_to_euler( qe );
    test_near( ee.x, 0.3f ); test_near( ee.y, 0.5f ); test_near( ee.z, -0.2f );

    quat_t qm = quat_from_mat4( quat_to_mat4( qe ) );
    test_near( f32_abs( quat_dot( qm, qe ) ), 1.0f );
}

/*==============================================================================================
    math: geometry & intersection
==============================================================================================*/

static void
test_math_geo( void )
{
    // rect2
    rect2_t r = rect2_from_center_size( vec2_make( 0, 0 ), vec2_make( 4, 4 ) );
    test_true( rect2_contains_point( r, vec2_make( 1, 1 ) ) );
    test_false( rect2_contains_point( r, vec2_make( 3, 0 ) ) );
    test_true( rect2_intersects( r, rect2_make( vec2_make( 1, 1 ), vec2_make( 5, 5 ) ) ) );

    // aabb basics + merge
    aabb_t b = aabb_from_center_extents( vec3_zero(), vec3_splat( 1 ) );      // [-1,1]^3
    test_true( aabb_contains_point( b, vec3_make( 0.5f, -0.5f, 0 ) ) );
    test_false( aabb_contains_point( b, vec3_make( 2, 0, 0 ) ) );
    aabb_t bm = aabb_merge_point( b, vec3_make( 3, 0, 0 ) );
    test_near( bm.max.x, 3.0f );

    // aabb transform: translate the box by (10,0,0)
    aabb_t bt = aabb_transform( b, mat4_translation( vec3_make( 10, 0, 0 ) ) );
    test_near( aabb_center( bt ).x, 10.0f );
    test_near( aabb_extents( bt ).x, 1.0f );

    // sphere
    sphere_t sp = sphere_make( vec3_zero(), 2.0f );
    test_true( sphere_contains_point( sp, vec3_make( 1, 1, 0 ) ) );
    test_true( sphere_intersects_aabb( sp, aabb_from_center_extents( vec3_make( 2.5f, 0, 0 ), vec3_splat( 1 ) ) ) );
    test_false( sphere_intersects_aabb( sp, aabb_from_center_extents( vec3_make( 5, 0, 0 ), vec3_splat( 1 ) ) ) );

    // ray vs aabb: shoot down -X into the box from +X
    f32   ht;
    ray_t rx = ray_make( vec3_make( 5, 0, 0 ), vec3_make( -1, 0, 0 ) );
    test_true( ray_vs_aabb( rx, b, &ht ) );
    test_near( ht, 4.0f );                                            // hits x=1 after 4 units
    ray_t miss = ray_make( vec3_make( 5, 5, 0 ), vec3_make( -1, 0, 0 ) );
    test_false( ray_vs_aabb( miss, b, &ht ) );

    // ray vs plane (z=0 plane), ray vs sphere, ray vs triangle
    plane_t pl = plane_from_point( vec3_make( 0, 0, 1 ), vec3_zero() );
    ray_t   rp = ray_make( vec3_make( 0, 0, 5 ), vec3_make( 0, 0, -1 ) );
    test_true( ray_vs_plane( rp, pl, &ht ) );
    test_near( ht, 5.0f );
    test_true( ray_vs_sphere( rp, sphere_make( vec3_zero(), 1.0f ), &ht ) );
    test_near( ht, 4.0f );
    b32 tri = ray_vs_triangle( rp, vec3_make( -1, -1, 0 ), vec3_make( 1, -1, 0 ), vec3_make( 0, 1, 0 ), &ht );
    test_true( tri );
    test_near( ht, 5.0f );

    // frustum: point in front of the camera is inside, behind is out, far is out
    mat4_t view = mat4_look_at( vec3_zero(), vec3_make( 0, 0, -1 ), vec3_make( 0, 1, 0 ) );
    mat4_t proj = mat4_perspective( MATH_PI_OVER_2, 1.0f, 0.1f, 100.0f );
    frustum_t fr = frustum_from_mat4( mat4_mul( proj, view ) );
    test_true( frustum_vs_point( fr, vec3_make( 0, 0, -5 ) ) );
    test_false( frustum_vs_point( fr, vec3_make( 0, 0, 5 ) ) );       // behind camera
    test_false( frustum_vs_point( fr, vec3_make( 0, 0, -500 ) ) );    // beyond far
    test_true( frustum_vs_sphere( fr, sphere_make( vec3_make( 0, 0, -5 ), 1.0f ) ) );
    test_true( frustum_vs_aabb( fr, aabb_from_center_extents( vec3_make( 0, 0, -5 ), vec3_splat( 1 ) ) ) );
}

/*==============================================================================================
    math: color
==============================================================================================*/

static void
test_math_color( void )
{
    // pack layout: R in the low byte, A in the high byte
    u32 c = color_rgba8( 255, 128, 64, 32 );
    test_equal( 255u, color_get_r( c ) );
    test_equal( 128u, color_get_g( c ) );
    test_equal( 64u, color_get_b( c ) );
    test_equal( 32u, color_get_a( c ) );

    // integer channels survive an unpack/pack round-trip
    test_equal( c, color_pack( color_unpack( c ) ) );
    test_near( color_unpack( c ).r, 1.0f );

    // with_alpha replaces only alpha
    test_equal( 200u, color_get_a( color_with_alpha( c, 200 ) ) );
    test_equal( 255u, color_get_r( color_with_alpha( c, 200 ) ) );

    // lerp midpoint of black->white is grey
    u32 mid = color_lerp( color_rgba8( 0, 0, 0, 255 ), color_rgba8( 255, 255, 255, 255 ), 0.5f );
    test_true( color_get_r( mid ) >= 127u && color_get_r( mid ) <= 128u );

    // sRGB <-> linear round-trip
    test_near( f32_linear_to_srgb( f32_srgb_to_linear( 0.5f ) ), 0.5f );

    // HSV <-> RGB: pure red round-trips; red is ( h=0, s=1, v=1 )
    vec3_t red = color_hsv_to_rgb( vec3_make( 0.0f, 1.0f, 1.0f ) );
    test_near( red.x, 1.0f ); test_near( red.y, 0.0f ); test_near( red.z, 0.0f );
    vec3_t hsv = color_rgb_to_hsv( vec3_make( 1, 0, 0 ) );
    test_near( hsv.y, 1.0f ); test_near( hsv.z, 1.0f );
    vec3_t rt = color_hsv_to_rgb( color_rgb_to_hsv( vec3_make( 0.2f, 0.6f, 0.4f ) ) );
    test_near( rt.x, 0.2f ); test_near( rt.y, 0.6f ); test_near( rt.z, 0.4f );
}

#undef test_near

/*==============================================================================================
    bit: popcount
==============================================================================================*/

static void
test_bit_popcount( void )
{
    test_equal( 0, bit_u32_popcount( 0u ) );
    test_equal( 1, bit_u32_popcount( 1u ) );
    test_equal( 1, bit_u32_popcount( 0x80000000u ) );
    test_equal( 4, bit_u32_popcount( 0xF0u ) );
    test_equal( 32, bit_u32_popcount( 0xFFFFFFFFu ) );

    test_equal( 0, bit_u64_popcount( 0ULL ) );
    test_equal( 16, bit_u64_popcount( 0xFFFF0000ULL ) );
    test_equal( 64, bit_u64_popcount( 0xFFFFFFFFFFFFFFFFULL ) );
}

/*==============================================================================================
    bit: CLZ and CTZ
==============================================================================================*/

static void
test_bit_clz_ctz( void )
{
    // CLZ (undefined for 0 � skip that case)
    test_equal( 31, bit_u32_clz( 1u ) );
    test_equal( 0, bit_u32_clz( 0x80000000u ) );
    test_equal( 16, bit_u32_clz( 0x0000FFFFu ) );
    test_equal( 24, bit_u32_clz( 0x000000FFu ) );

    test_equal( 63, bit_u64_clz( 1ULL ) );
    test_equal( 0, bit_u64_clz( 0x8000000000000000ULL ) );

    // CTZ (undefined for 0 � skip that case)
    test_equal( 0, bit_u32_ctz( 1u ) );
    test_equal( 4, bit_u32_ctz( 0x10u ) );
    test_equal( 8, bit_u32_ctz( 0x100u ) );
    test_equal( 31, bit_u32_ctz( 0x80000000u ) );

    test_equal( 0, bit_u64_ctz( 1ULL ) );
    test_equal( 32, bit_u64_ctz( 0x100000000ULL ) );
}

/*==============================================================================================
    bit: power-of-two helpers
==============================================================================================*/

static void
test_bit_pow2( void )
{
    test_true( bit_u32_is_pow2( 1u ) );
    test_true( bit_u32_is_pow2( 2u ) );
    test_true( bit_u32_is_pow2( 256u ) );
    test_false( bit_u32_is_pow2( 0u ) );
    test_false( bit_u32_is_pow2( 3u ) );
    test_false( bit_u32_is_pow2( 6u ) );

    // next_pow2: returns x if x is already a power of two
    test_equal( 1u, bit_u32_next_pow2( 0u ) );
    test_equal( 1u, bit_u32_next_pow2( 1u ) );
    test_equal( 2u, bit_u32_next_pow2( 2u ) );
    test_equal( 4u, bit_u32_next_pow2( 3u ) );
    test_equal( 8u, bit_u32_next_pow2( 5u ) );
    test_equal( 16u, bit_u32_next_pow2( 9u ) );
    test_equal( 256u, bit_u32_next_pow2( 129u ) );

    test_equal( 1ULL, bit_u64_next_pow2( 0ULL ) );
    test_equal( 8ULL, bit_u64_next_pow2( 5ULL ) );
    test_equal( 0x100000000ULL, bit_u64_next_pow2( 0x80000001ULL ) );
}

/*==============================================================================================
    bit: rotation
==============================================================================================*/

static void
test_bit_rotate( void )
{
    test_equal( 2u, bit_u32_rotl( 1u, 1 ) );
    test_equal( 0x80000000u, bit_u32_rotr( 1u, 1 ) );
    test_equal( 1u, bit_u32_rotl( 0x80000000u, 1 ) );    // wraps around
    test_equal( 0x40000000u, bit_u32_rotr( 0x80000000u, 1 ) );

    test_equal( 2ULL, bit_u64_rotl( 1ULL, 1 ) );
    test_equal( 0x8000000000000000ULL, bit_u64_rotr( 1ULL, 1 ) );
}

/*==============================================================================================
    bit: field access and flag helpers
==============================================================================================*/

static void
test_bit_fields_flags( void )
{
    // bit_field_get: extract 4 bits at position 4 from 0xAB (0b10101011)
    // bits [7:4] = 0b1010 = 10
    test_equal( 10u, bit_field_get( 0xABu, 4, 4 ) );

    // extract 3 bits at position 4 from 0xF0 (0b11110000)
    // bits [6:4] = 0b111 = 7
    test_equal( 7u, bit_field_get( 0xF0u, 4, 3 ) );

    // bit_field_set: in 0x00, set 3 bits at position 4 to 5 (0b101)
    // result: 0b01010000 = 0x50
    test_equal( 0x50u, bit_field_set( 0u, 4, 3, 5u ) );

    // in 0xFF, set 3 bits at position 4 to 5: mask out [6:4] then OR in 5
    // 0xFF & ~0x70 = 0x8F; 0x8F | 0x50 = 0xDF
    test_equal( 0xDFu, bit_field_set( 0xFFu, 4, 3, 5u ) );

    // bit flags
    u32 flags = 0;
    bit_flag_set( flags, 0x01u );
    test_true( bit_flag_has( flags, 0x01u ) );
    test_false( bit_flag_has( flags, 0x02u ) );

    bit_flag_set( flags, 0x04u );
    test_true( bit_flag_has_all( flags, 0x01u | 0x04u ) );
    test_false( bit_flag_has_all( flags, 0x01u | 0x08u ) );

    bit_flag_clear( flags, 0x01u );
    test_false( bit_flag_has( flags, 0x01u ) );
    test_true( bit_flag_has( flags, 0x04u ) );

    bit_flag_toggle( flags, 0x04u );
    test_false( bit_flag_has( flags, 0x04u ) );
    bit_flag_toggle( flags, 0x04u );
    test_true( bit_flag_has( flags, 0x04u ) );
}

/*==============================================================================================
    container: dynamic array
==============================================================================================*/

#if ORB_USE_CONTAINERS

da_typedef( i32, int_array_t );

static void
test_container_append( void )
{
    int_array_t xs = { 0 };

    test_true( da_empty( xs ) );

    for ( i32 i = 0; i < 20; i++ ) da_append( xs, i * 2 );

    test_equal( 20, xs.count );
    test_true( xs.capacity >= xs.count );
    test_false( da_empty( xs ) );
    test_equal( 0, xs.items[ 0 ] );
    test_equal( 38, xs.items[ 19 ] );
    test_equal( 38, da_last( xs ) );

    da_free( xs );
    test_null( xs.items );
    test_equal( 0, xs.count );
    test_equal( 0, xs.capacity );
}

static void
test_container_append_many( void )
{
    int_array_t xs       = { 0 };
    i32         src[ 5 ] = { 10, 11, 12, 13, 14 };

    da_append( xs, 1 );
    da_append_many( xs, src, 5 );

    test_equal( 6, xs.count );
    test_equal( 1, xs.items[ 0 ] );
    test_equal( 10, xs.items[ 1 ] );
    test_equal( 14, xs.items[ 5 ] );

    da_free( xs );
}

static void
test_container_insert_remove( void )
{
    int_array_t xs = { 0 };

    for ( i32 i = 0; i < 5; i++ ) da_append( xs, i );    // 0 1 2 3 4

    da_insert( xs, 2, 99 );    // 0 1 99 2 3 4
    test_equal( 6, xs.count );
    test_equal( 99, xs.items[ 2 ] );
    test_equal( 2, xs.items[ 3 ] );
    test_equal( 4, xs.items[ 5 ] );

    da_remove_ordered( xs, 2 );    // 0 1 2 3 4
    test_equal( 5, xs.count );
    test_equal( 2, xs.items[ 2 ] );
    test_equal( 3, xs.items[ 3 ] );
    test_equal( 4, xs.items[ 4 ] );

    da_remove_swap( xs, 0 );    // 4 1 2 3
    test_equal( 4, xs.count );
    test_equal( 4, xs.items[ 0 ] );

    test_equal( 3, da_pop( xs ) );
    test_equal( 3, xs.count );

    da_clear( xs );
    test_equal( 0, xs.count );
    test_true( xs.capacity > 0 );    // clear keeps the allocation

    da_free( xs );
}

#endif 
/*==============================================================================================
    Entry point
==============================================================================================*/

int
base_run_tests( void )
{
    test_str();

    test_register( "char_classify", test_char_classify );
    test_register( "char_convert", test_char_convert );

    test_register( "mem_copy_move", test_mem_copy_move );
    test_register( "mem_set_zero", test_mem_set_zero );
    test_register( "mem_compare", test_mem_compare );
    test_register( "mem_swap_reverse", test_mem_swap_reverse );
    test_register( "mem_align", test_mem_align );

    test_register( "math_minmax_clamp", test_math_minmax_clamp );
    test_register( "math_abs", test_math_abs );
    test_register( "math_lerp", test_math_lerp );
    test_register( "math_sign_align", test_math_sign_align );
    test_register( "math_rng", test_math_rng );
    test_register( "math_scalar", test_math_scalar );
    test_register( "math_ease", test_math_ease );
    test_register( "math_vec", test_math_vec );
    test_register( "math_mat", test_math_mat );
    test_register( "math_quat", test_math_quat );
    test_register( "math_extras", test_math_extras );
    test_register( "math_geo", test_math_geo );
    test_register( "math_color", test_math_color );

    test_register( "bit_popcount", test_bit_popcount );
    test_register( "bit_clz_ctz", test_bit_clz_ctz );
    test_register( "bit_pow2", test_bit_pow2 );
    test_register( "bit_rotate", test_bit_rotate );
    test_register( "bit_fields_flags", test_bit_fields_flags );

    #if ORB_USE_CONTAINERS
    test_register( "container_append", test_container_append );
    test_register( "container_append_many", test_container_append_many );
    test_register( "container_insert_remove", test_container_insert_remove );
    #endif

    return test_run( "base" );
}

/*============================================================================================*/