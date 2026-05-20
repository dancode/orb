/*==============================================================================================

    base/bit.h -- Bit manipulation utilities.

        Uses compiler intrinsics when available (MSVC / GCC / Clang).
        Most functions are ORB_INLINE for maximum performance in release builds.

==============================================================================================*/
#ifndef BIT_H
#define BIT_H

// clang-format off
/*==============================================================================================
    Intrinsics Configuration
==============================================================================================*/

#if COMPILER_MSVC
    #include <intrin.h>
    #pragma intrinsic( _rotl, _rotr, _rotl64, _rotr64 )
#endif

/*==============================================================================================
    Population count (number of set bits)
==============================================================================================*/

ORB_INLINE i32
bit_u32_popcount( u32 x )
{
#if COMPILER_MSVC
    return ( i32 )__popcnt( x );
#else
    return __builtin_popcount( x );
#endif
}

ORB_INLINE i32
bit_u64_popcount( u64 x )
{
#if COMPILER_MSVC
    return ( i32 )__popcnt64( x );
#else
    return __builtin_popcountll( x );
#endif
}

/*==============================================================================================
    Count Leading Zeros (CLZ)
==============================================================================================*/

// Returns the number of leading zeros.
// If x is 0, the result is 32.
ORB_INLINE i32
bit_u32_clz( u32 x )
{
    if ( x == 0 ) return 32;
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanReverse( &idx, x );
    return ( i32 )( 31 - idx );
#else
    return __builtin_clz( x );
#endif
}

// Returns the number of leading zeros.
// If x is 0, the result is 64.
ORB_INLINE i32
bit_u64_clz( u64 x )
{
    if ( x == 0 ) return 64;
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanReverse64( &idx, x );
    return ( i32 )( 63 - idx );
#else
    return __builtin_clzll( x );
#endif
}

/*==============================================================================================
    Count Trailing Zeros (CTZ)
==============================================================================================*/

// Returns the number of trailing zeros.
// If x is 0, the result is 32.
ORB_INLINE i32
bit_u32_ctz( u32 x )
{
    if ( x == 0 ) return 32;
#if COMPILER_MSVC
    unsigned long idx;
    _BitScanForward( &idx, x );
    return ( i32 )idx;
#else
    return __builtin_ctz( x );
#endif
}

// Returns the number of trailing zeros.
// If x is 0, the result is 64.
ORB_INLINE i32
bit_u64_ctz( u64 x )
{
    if ( x == 0 ) return 64;
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

ORB_INLINE b32
bit_u32_is_pow2( u32 x )
{
    return x != 0 && ( x & ( x - 1 ) ) == 0;
}

ORB_INLINE b32
bit_u64_is_pow2( u64 x )
{
    return x != 0 && ( x & ( x - 1 ) ) == 0;
}

ORB_INLINE u32
bit_u32_next_pow2( u32 x )
{
    if ( x <= 1 ) return 1;
    return 1u << ( 32 - bit_u32_clz( x - 1 ) );
}

ORB_INLINE u64
bit_u64_next_pow2( u64 x )
{
    if ( x <= 1 ) return 1;
    return 1ULL << ( 64 - bit_u64_clz( x - 1 ) );
}

/*==============================================================================================
    Rotation
==============================================================================================*/

ORB_INLINE u32
bit_u32_rotl( u32 x, i32 n )
{
#if COMPILER_MSVC
    return _rotl( x, n );
#else
    return ( x << n ) | ( x >> ( ( 32 - n ) & 31 ) );
#endif
}

ORB_INLINE u32
bit_u32_rotr( u32 x, i32 n )
{
#if COMPILER_MSVC
    return _rotr( x, n );
#else
    return ( x >> n ) | ( x << ( ( 32 - n ) & 31 ) );
#endif
}

ORB_INLINE u64
bit_u64_rotl( u64 x, i32 n )
{
#if COMPILER_MSVC
    return _rotl64( x, n );
#else
    return ( x << n ) | ( x >> ( ( 64 - n ) & 63 ) );
#endif
}

ORB_INLINE u64
bit_u64_rotr( u64 x, i32 n )
{
#if COMPILER_MSVC
    return _rotr64( x, n );
#else
    return ( x >> n ) | ( x << ( ( 64 - n ) & 63 ) );
#endif
}

/*==============================================================================================
    Bit field access
==============================================================================================*/

// Extract bits [lo, lo+count) from x.
#define bit_field_get( x, lo, count ) ( ( ( x ) >> ( lo ) ) & ( ( 1ULL << ( count ) ) - 1ULL ) )

// Set bits [lo, lo+count) in x to val.

#define bit_field_set( x, lo, count, val )                         \
    ( ( ( x ) & ~( ( ( 1ULL << ( count ) ) - 1ULL ) << ( lo ) ) ) | \
      ( ( ( u64 )( val ) & ( ( 1ULL << ( count ) ) - 1ULL ) ) << ( lo ) ) )

/*==============================================================================================
    Flag helpers
==============================================================================================*/

#define bit_flag_set( flags, f )     ( ( flags ) |= ( f ) )
#define bit_flag_clear( flags, f )   ( ( flags ) &= ~( f ) )
#define bit_flag_toggle( flags, f )  ( ( flags ) ^= ( f ) )
#define bit_flag_has( flags, f )     ( ( ( flags ) & ( f ) ) != 0 )
#define bit_flag_has_all( flags, f ) ( ( ( flags ) & ( f ) ) == ( f ) )

// clang-format off
/*============================================================================================*/
#endif // BIT_H