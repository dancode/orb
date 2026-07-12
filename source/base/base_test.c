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
    } vec3i_t;

    vec3i_t v = { 1, 2, 3.0f };
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