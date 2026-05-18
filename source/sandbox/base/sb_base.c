/*==============================================================================================

    sandbox/base/sb_base.c -- Sandbox for the base library.

    Links against the base static library and runs the full test suite.
    Exit code: 0 = all tests passed, non-zero = failures.

==============================================================================================*/

#include "base/base.h"
#include "base/test.h"

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
    str: length and empty
==============================================================================================*/

static void
test_str_len_empty( void )
{
    test_equal( 0, str_len( "" ) );
    test_equal( 0, str_len( NULL ) );
    test_equal( 5, str_len( "hello" ) );
    test_equal( 3, str_len( "abc" ) );
    test_equal( 1, str_len( "x" ) );

    test_true( str_empty( "" ) );
    test_true( str_empty( NULL ) );
    test_false( str_empty( "a" ) );
    test_false( str_empty( "hello" ) );
}

/*==============================================================================================
    str: copy and append
==============================================================================================*/

static void
test_str_copy_append( void )
{
    char buf[ 16 ];

    // Normal copy: returns src length
    usize r = str_copy( buf, sizeof( buf ), "hello" );
    test_equal( 5, r );
    test_str_equal( "hello", buf );

    // Copy with truncation: returns src length, not copied amount
    r = str_copy( buf, 4, "hello" );
    test_equal( 5, r );
    test_str_equal( "hel", buf );    // 3 chars + null

    // Empty src
    r = str_copy( buf, sizeof( buf ), "" );
    test_equal( 0, r );
    test_str_equal( "", buf );

    // Append to existing content
    str_copy( buf, sizeof( buf ), "hello" );
    r = str_append( buf, sizeof( buf ), " world" );
    test_equal( 11, r );
    test_str_equal( "hello world", buf );

    // Append: "hello world!" (12 chars) + "xyz" (3) = 15 chars, fits exactly in 16-byte buf
    char buf2[ 16 ];
    str_copy( buf2, sizeof( buf2 ), "hello world!" );
    str_append( buf2, sizeof( buf2 ), "xyz" );
    test_str_equal( "hello world!xyz", buf2 );
}

/*==============================================================================================
    str: comparison
==============================================================================================*/

static void
test_str_compare( void )
{
    test_true( str_equal( "abc", "abc" ) );
    test_false( str_equal( "abc", "ABC" ) );
    test_true( str_equal( "", "" ) );
    test_true( str_equal( NULL, NULL ) );
    test_false( str_equal( NULL, "a" ) );
    test_false( str_equal( "a", NULL ) );

    test_equal( 0, str_cmp( "abc", "abc" ) );
    test_true( str_cmp( "abc", "abd" ) < 0 );
    test_true( str_cmp( "abd", "abc" ) > 0 );
    test_true( str_cmp( "a", "ab" ) < 0 );
    test_equal( 0, str_cmp( NULL, NULL ) );

    test_equal( 0, str_ncmp( "hello", "hello!", 5 ) );
    test_true( str_ncmp( "abc", "abd", 3 ) < 0 );
    test_equal( 0, str_ncmp( "abc", "abc", 0 ) );

    test_true( str_equal_nocase( "Hello", "hello" ) );
    test_true( str_equal_nocase( "ABC", "abc" ) );
    test_true( str_equal_nocase( "MiXeD", "mixed" ) );
    test_false( str_equal_nocase( "abc", "abd" ) );
    test_false( str_equal_nocase( "abc", "abcd" ) );
}

/*==============================================================================================
    str: search
==============================================================================================*/

static void
test_str_search( void )
{
    const char* s = "hello world";

    // find_char
    test_not_null( str_find_char( s, 'h' ) );
    test_not_null( str_find_char( s, 'o' ) );
    test_null( str_find_char( s, 'x' ) );
    test_equal( 'h', *str_find_char( s, 'h' ) );

    // rfind_char: last 'l' is at index 9 (0-based)
    const char* r = str_rfind_char( s, 'l' );
    test_not_null( r );
    test_equal( 'l', *r );
    test_true( r > s + 5 );

    // find_sub
    test_not_null( str_find_sub( s, "world" ) );
    test_not_null( str_find_sub( s, "hello" ) );
    test_null( str_find_sub( s, "xyz" ) );
    test_not_null( str_find_sub( s, "" ) );    // empty needle returns haystack

    // starts_with / ends_with
    test_true( str_starts_with( s, "hello" ) );
    test_false( str_starts_with( s, "world" ) );
    test_true( str_starts_with( s, "" ) );

    test_true( str_ends_with( s, "world" ) );
    test_false( str_ends_with( s, "hello" ) );
    test_true( str_ends_with( "hello", "hello" ) );    // equal strings
    test_true( str_ends_with( s, "" ) );
}

/*==============================================================================================
    str: hash
==============================================================================================*/

static void
test_str_hash( void )
{
    // Deterministic
    test_equal( str_hash( "hello" ), str_hash( "hello" ) );

    // Different inputs produce different hashes
    test_not_equal( str_hash( "hello" ), str_hash( "world" ) );
    test_not_equal( str_hash( "abc" ), str_hash( "ABC" ) );
    test_not_equal( str_hash( "" ), str_hash( "a" ) );

    // hash_n matches hash on first n bytes
    test_equal( str_hash( "abc" ), str_hash_n( "abcxyz", 3 ) );
    test_equal( str_hash( "" ), str_hash_n( "hello", 0 ) );
}

/*==============================================================================================
    str: parsing
==============================================================================================*/

static void
test_str_parse( void )
{
    long long          iv;
    unsigned long long uv;

    // i64: returns chars consumed
    test_equal( 3, str_parse_i64( "123", &iv ) );
    test_equal( 123, iv );

    test_equal( 4, str_parse_i64( "-456", &iv ) );
    test_equal( -456, iv );

    test_equal( 2, str_parse_i64( "+7", &iv ) );
    test_equal( 7, iv );

    test_equal( 2, str_parse_i64( "99x", &iv ) );    // stops at non-digit, consumed 2 digits
    test_equal( 99, iv );

    test_equal( 0, str_parse_i64( "abc", &iv ) );    // no digits
    test_equal( 0, str_parse_i64( NULL, &iv ) );

    // u64
    test_equal( 3, str_parse_u64( "789", &uv ) );
    test_equal( 789, uv );

    test_equal( 0, str_parse_u64( "abc", &uv ) );
    test_equal( 0, str_parse_u64( NULL, &uv ) );
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
    test_equal( 3, math_i32_min( 3, 5 ) );
    test_equal( 3, math_i32_min( 5, 3 ) );
    test_equal( 5, math_i32_max( 3, 5 ) );
    test_equal( 5, math_i32_max( 5, 3 ) );
    test_equal( -5, math_i32_min( -5, 3 ) );
    test_equal( 3, math_i32_max( -5, 3 ) );

    test_equal( 3u, math_u32_min( 3u, 5u ) );
    test_equal( 5u, math_u32_max( 3u, 5u ) );

    test_equal( 5, math_i32_clamp( 5, 0, 10 ) );
    test_equal( 0, math_i32_clamp( -1, 0, 10 ) );
    test_equal( 10, math_i32_clamp( 20, 0, 10 ) );
    test_equal( 0, math_i32_clamp( 0, 0, 10 ) );
    test_equal( 10, math_i32_clamp( 10, 0, 10 ) );

    test_equal( 5u, math_u32_clamp( 5u, 0u, 10u ) );
    test_equal( 0u, math_u32_clamp( 0u, 0u, 10u ) );
    test_equal( 10u, math_u32_clamp( 99u, 0u, 10u ) );
}

/*==============================================================================================
    math: abs
==============================================================================================*/

static void
test_math_abs( void )
{
    test_equal( 5, math_i32_abs( 5 ) );
    test_equal( 5, math_i32_abs( -5 ) );
    test_equal( 0, math_i32_abs( 0 ) );

    test_equal( 5, math_i64_abs( -5LL ) );
    test_equal( 0, math_i64_abs( 0LL ) );

    test_true( math_f32_abs( -3.14f ) > 0.0f );
    test_true( math_f32_nearly_equal( math_f32_abs( -3.14f ), 3.14f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_abs( 3.14f ), 3.14f, F32_EPSILON ) );
}

/*==============================================================================================
    math: lerp, unlerp, remap
==============================================================================================*/

static void
test_math_lerp( void )
{
    test_true( math_f32_nearly_equal( math_f32_lerp( 0.0f, 10.0f, 0.0f ), 0.0f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_lerp( 0.0f, 10.0f, 0.5f ), 5.0f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_lerp( 0.0f, 10.0f, 1.0f ), 10.0f, F32_EPSILON ) );

    test_true( math_f32_nearly_equal( math_f32_unlerp( 0.0f, 10.0f, 0.0f ), 0.0f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_unlerp( 0.0f, 10.0f, 5.0f ), 0.5f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_unlerp( 0.0f, 10.0f, 10.0f ), 1.0f, F32_EPSILON ) );

    // remap: 5 in [0,10] maps to 50 in [0,100]
    test_true( math_f32_nearly_equal( math_f32_remap( 0.0f, 10.0f, 0.0f, 100.0f, 5.0f ), 50.0f, 1e-4f ) );
}

/*==============================================================================================
    math: sign and align
==============================================================================================*/

static void
test_math_sign_align( void )
{
    test_equal( 1, math_i32_sign( 5 ) );
    test_equal( -1, math_i32_sign( -5 ) );
    test_equal( 0, math_i32_sign( 0 ) );

    test_true( math_f32_nearly_equal( math_f32_sign( 3.14f ), 1.0f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_sign( -1.0f ), -1.0f, F32_EPSILON ) );
    test_true( math_f32_nearly_equal( math_f32_sign( 0.0f ), 0.0f, F32_EPSILON ) );

    test_equal( 8, math_align_up( 5, 8 ) );
    test_equal( 8, math_align_up( 8, 8 ) );    // already aligned
    test_equal( 16, math_align_up( 9, 8 ) );
    test_equal( 8, math_align_down( 13, 8 ) );
    test_equal( 8, math_align_down( 8, 8 ) );    // already aligned
    test_equal( 16, math_align_down( 23, 16 ) );
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
    test_equal( 31, bit_u32_clz2( 1u ) );
    test_equal( 0, bit_u32_clz2( 0x80000000u ) );
    test_equal( 16, bit_u32_clz2( 0x0000FFFFu ) );
    test_equal( 24, bit_u32_clz2( 0x000000FFu ) );

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
    test_equal( 1u, bit_u32_next_pow22( 0u ) );
    test_equal( 1u, bit_u32_next_pow22( 1u ) );
    test_equal( 2u, bit_u32_next_pow22( 2u ) );
    test_equal( 4u, bit_u32_next_pow22( 3u ) );
    test_equal( 8u, bit_u32_next_pow22( 5u ) );
    test_equal( 16u, bit_u32_next_pow22( 9u ) );
    test_equal( 256u, bit_u32_next_pow22( 129u ) );

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
    fmt: integer formatting
==============================================================================================*/

static void
test_fmt_integers( void )
{
    char buf[ 32 ];

    // fmt_i64
    fmt_i64( buf, sizeof( buf ), 0 );
    test_str_equal( "0", buf );

    fmt_i64( buf, sizeof( buf ), 12345 );
    test_str_equal( "12345", buf );

    fmt_i64( buf, sizeof( buf ), -99 );
    test_str_equal( "-99", buf );

    fmt_i64( buf, sizeof( buf ), -1 );
    test_str_equal( "-1", buf );

    // fmt_u64
    fmt_u64( buf, sizeof( buf ), 0u );
    test_str_equal( "0", buf );

    fmt_u64( buf, sizeof( buf ), 999u );
    test_str_equal( "999", buf );

    fmt_u64( buf, sizeof( buf ), 4294967295u );    // UINT32_MAX
    test_str_equal( "4294967295", buf );

    // fmt_hex64
    fmt_hex64( buf, sizeof( buf ), 0u );
    test_str_equal( "0", buf );

    fmt_hex64( buf, sizeof( buf ), 0xABCu );
    test_str_equal( "abc", buf );

    fmt_hex64( buf, sizeof( buf ), 0xDEADBEEFu );
    test_str_equal( "deadbeef", buf );
}

/*==============================================================================================
    fmt: append
==============================================================================================*/

static void
test_fmt_append( void )
{
    char buf[ 64 ];
    i32  pos = 0;

    pos += fmt_append( buf, sizeof( buf ), pos, "Hello" );
    pos += fmt_append( buf, sizeof( buf ), pos, " %s", "world" );
    test_str_equal( "Hello world", buf );
    test_equal( 11, pos );

    // Append an integer
    pos += fmt_append( buf, sizeof( buf ), pos, " %d", 42 );
    test_str_equal( "Hello world 42", buf );

    // Truncation: buf already has content up to pos, try writing more than fits
    char buf2[ 8 ];
    i32  p = 0;
    p += fmt_append( buf2, sizeof( buf2 ), p, "Hello" );    // 5 bytes
    p += fmt_append( buf2, sizeof( buf2 ), p, "XYZ" );      // 2 fit (cap-1=7, 7-5=2)
    test_str_equal( "HelloXY", buf2 );
}

/*==============================================================================================
    Entry point
==============================================================================================*/

int
base_run_tests( void )
{
    test_register( "char_classify", test_char_classify );
    test_register( "char_convert", test_char_convert );
    test_register( "str_len_empty", test_str_len_empty );
    test_register( "str_copy_append", test_str_copy_append );
    test_register( "str_compare", test_str_compare );
    test_register( "str_search", test_str_search );
    test_register( "str_hash", test_str_hash );
    test_register( "str_parse", test_str_parse );
    test_register( "mem_copy_move", test_mem_copy_move );
    test_register( "mem_set_zero", test_mem_set_zero );
    test_register( "mem_compare", test_mem_compare );
    test_register( "mem_swap_reverse", test_mem_swap_reverse );
    test_register( "mem_align", test_mem_align );
    test_register( "math_minmax_clamp", test_math_minmax_clamp );
    test_register( "math_abs", test_math_abs );
    test_register( "math_lerp", test_math_lerp );
    test_register( "math_sign_align", test_math_sign_align );
    test_register( "bit_popcount", test_bit_popcount );
    test_register( "bit_clz_ctz", test_bit_clz_ctz );
    test_register( "bit_pow2", test_bit_pow2 );
    test_register( "bit_rotate", test_bit_rotate );
    test_register( "bit_fields_flags", test_bit_fields_flags );
    test_register( "fmt_integers", test_fmt_integers );
    test_register( "fmt_append", test_fmt_append );

    return test_run( "base" );
}

/*============================================================================================*/

int
main( int argc, char* argv[] )
{
    UNUSED( argc );
    UNUSED( argv );
    return base_run_tests();
}

/*============================================================================================*/
