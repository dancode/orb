/*==============================================================================================

    bit.h -- Bit manipulation utilities.

        Uses compiler intrinsics when available (MSVC / GCC / Clang).

==============================================================================================*/
#ifndef BIT_H
#define BIT_H
/*==============================================================================================
    Population count  (number of set bits)
==============================================================================================*/

static inline i32
bit_u32_popcount( u32 x )
{
    /* Counts the number of set bits in a 32-bit integer. */
#if COMPILER_MSVC
    return ( i32 )__popcnt( x );
#else
    return __builtin_popcount( x );
#endif
}

static inline i32
bit_u64_popcount( u64 x )
{
    /* Counts the number of set bits in a 64-bit integer. */
#if COMPILER_MSVC
    return ( i32 )__popcnt64( x );
#else
    return __builtin_popcountll( x );
#endif
}

/*==============================================================================================
    Count Leading Zeros  (undefined behaviour if x == 0)
==============================================================================================*/

static inline i32
bit_u32_clz2( u32 x )
{
    /* Returns the number of leading zeros in the binary representation of x. */
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanReverse( &idx, x );
    return ( i32 )( 31 - idx );
#else
    return __builtin_clz( x );
#endif
}

static inline i32
bit_u64_clz( u64 x )
{
    /* Returns the number of leading zeros in the binary representation of x. */
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanReverse64( &idx, x );
    return ( i32 )( 63 - idx );
#else
    return __builtin_clzll( x );
#endif
}

/*==============================================================================================
    Count Trailing Zeros  (undefined behaviour if x == 0)
==============================================================================================*/


static inline i32
bit_u32_ctz( u32 x )
{
    /* Returns the number of trailing zeros in the binary representation of x. */
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanForward( &idx, x );
    return ( i32 )idx;
#else
    return __builtin_ctz( x );
#endif
}

static inline i32
bit_u64_ctz( u64 x )
{
    /* Returns the number of trailing zeros in the binary representation of x. */
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanForward64( &idx, x );
    return ( i32 )idx;
#else
    return __builtin_ctzll( x );
#endif
}

/*==============================================================================================
    Power-of-two helpers
==============================================================================================*/

static inline b32
bit_u32_is_pow2( u32 x )
{
    // Returns 1 if x is a power of two (x must be > 0).
    return x != 0 && ( x & ( x - 1 ) ) == 0;
}

static inline b32
bit_u64_is_pow2( u64 x )
{
    // Returns 1 if x is a power of two (x must be > 0).
    return x != 0 && ( x & ( x - 1 ) ) == 0;
}

static inline u32
bit_u32_next_pow22( u32 x )
{
    // Round x up to next power of two (returns x if already a power of two).
    // Input 0 returns 1. Input > 2^31 is undefined for u32.

    if ( x == 0 )
        return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

static inline u64
bit_u64_next_pow2( u64 x )
{
    // Round x up to next power of two (returns x if already a power of two).
    // Input 0 returns 1. Input > 2^63 is undefined for u64.

    if ( x == 0 )
        return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

/*==============================================================================================
    Rotation
==============================================================================================*/

static inline u32
bit_u32_rotl( u32 x, i32 n )
{
    return ( x << n ) | ( x >> ( 32 - n ) );
}

static inline u32
bit_u32_rotr( u32 x, i32 n )
{
    return ( x >> n ) | ( x << ( 32 - n ) );
}

static inline u64 
bit_u64_rotl( u64 x, i32 n ) {
    return ( x << n ) | ( x >> ( 64 - n ) );
}

static inline u64
bit_u64_rotr( u64 x, i32 n )
{
    return ( x >> n ) | ( x << ( 64 - n ) );
}

/*==============================================================================================
     Byte-swap  (endianness) note: We currently do not care about endianness.
==============================================================================================*/

/*
#include <stdlib.h> // required
 
static inline u32
bit_u32_bswap( u32 x )
{
    // Returns the value of x with byte order reversed (e.g. 0x12345678 becomes 0x78563412). 
#if COMPILER_MSVC
    return _byteswap_ulong( x );
#else
    return __builtin_bswap32( x );
#endif
}

static inline u64
bit_u64_bswap( u64 x )
{
    // Note: MSVC's _byteswap_uint64 is only available in 64-bit mode, so we can't use it on 32-bit MSVC targets.
#if COMPILER_MSVC
    return _byteswap_uint64( x );
#else
    return __builtin_bswap64( x );
#endif
}
*/

/*==============================================================================================
     Bit field access
==============================================================================================*/

// Extract bits [lo, lo+count) from x.
// For example, bit_field_get( 0b10 110 100, 2, 3 ) returns 0b110 (the bits in positions 2, 3, and 4).
#define bit_field_get( x, lo, count ) ( ( ( x ) >> ( lo ) ) & ( ( 1u << ( count ) ) - 1u ) )

/* Set bits [lo, lo+count) in x to val (val is not range-checked).
   Ex: bit_field_set( 0b10 000 100, 2, 3, 0b110 ) 
       returns 0b10 110 100 (the bits in positions 2, 3, and 4 are set to 0b110).*/

#define bit_field_set( x, lo, count, val )                      \
    ( ( ( x ) & ~( ( ( 1u << ( count ) ) - 1u ) << ( lo ) ) ) | \
      ( ( ( val ) & ( ( 1u << ( count ) ) - 1u ) ) << ( lo ) ) )

/*==============================================================================================
     Flag helpers  (for u32 flag sets)

    These macros treat flags as a set of bits in an integer. For example, if you have:

        enum MyFlags { FLAG_A = BIT(0), FLAG_B = BIT(1), FLAG_C = BIT(2),};

    These macros to manipulate a type u32 that holds a combination of these flags:

    u32 flags = 0;
    bit_flag_set( flags, FLAG_A | FLAG_C );             // Set FLAG_A and FLAG_C
    if (bit_flag_has(flags, FLAG_B )) {}                // Check if FLAG_B is set
    bit_flag_clear( flags, FLAG_A );                    // Clear FLAG_A
    bit_flag_toggle( flags, FLAG_C );                   // Toggle FLAG_C
    if ( bit_flag_has_all( flags, FLAG_A | FLAG_C )) {} // Check if both FLAG_A and FLAG_C are set

==============================================================================================*/

#define bit_flag_set( flags, f )     ( ( flags ) |= ( f ) )
#define bit_flag_clear( flags, f )   ( ( flags ) &= ~( f ) )
#define bit_flag_toggle( flags, f )  ( ( flags ) ^= ( f ) )
#define bit_flag_has( flags, f )     ( ( ( flags ) & ( f ) ) != 0 )
#define bit_flag_has_all( flags, f ) ( ( ( flags ) & ( f ) ) == ( f ) )

/*============================================================================================*/
#endif    // BIT_H