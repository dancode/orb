/*==============================================================================================

    base/mem.h -- Memory operations (NO allocation).
        
        Uses compiler intrinsics when available (MSVC / GCC / Clang).
        Most functions are ORB_INLINE for maximum performance in release builds.

==============================================================================================*/
#ifndef MEM_H
#define MEM_H
/*==============================================================================================
    Intrinsics Configuration
==============================================================================================*/

// Compiler intrinsics for memory operations to avoid <string.h>
#if defined( _MSC_VER )
    // Use the SAL-annotated MSVC declarations to satisfy static analysis (C28251).
    #include <string.h>
    #pragma intrinsic( memcpy, memmove, memset, memcmp )
#else
    // Clang / GCC
    #define memcpy  __builtin_memcpy
    #define memmove __builtin_memmove
    #define memset  __builtin_memset
    #define memcmp  __builtin_memcmp
#endif

/*==============================================================================================
    Copy and Move
==============================================================================================*/

// Copy n bytes from src to dst. Regions must NOT overlap.
ORB_INLINE void
mem_copy( void* dst, const void* src, usize n )
{
    memcpy( dst, src, n );
}

// Copy n bytes, handling overlapping regions correctly.
ORB_INLINE void
mem_move( void* dst, const void* src, usize n )
{
    memmove( dst, src, n );
}

/*==============================================================================================
    Fill
==============================================================================================*/

// Fill n bytes at dst with value.
ORB_INLINE void
mem_set( void* dst, u8 value, usize n )
{
    memset( dst, ( int )value, n );
}

// Zero n bytes at dst.
ORB_INLINE void
mem_zero( void* dst, usize n )
{
    mem_set( dst, 0, n );
}

// Zero a single typed value via pointer.
#define mem_zero_struct( ptr ) mem_zero( ( ptr ), sizeof( *( ptr ) ) )

// Zero an array of count elements (ptr must be a typed pointer).
#define mem_zero_array( ptr, count ) mem_zero( ( ptr ), sizeof( *( ptr ) ) * ( count ) )

/*==============================================================================================
    Comparison
==============================================================================================*/

// Returns 1 if first n bytes of a and b are identical, 0 otherwise.
ORB_INLINE b32
mem_equal( const void* a, const void* b, usize n )
{
    return memcmp( a, b, n ) == 0;
}

// Lexicographic compare; returns <0, 0, or >0 (same contract as memcmp).
ORB_INLINE int
mem_compare( const void* a, const void* b, usize n )
{
    return memcmp( a, b, n );
}

/*==============================================================================================
    Swap and Reverse
==============================================================================================*/

// Swap n bytes between a and b. Buffers must not overlap.
void mem_swap( void* a, void* b, usize n );

// Reverse n bytes in-place.
void mem_reverse( void* buf, usize n );

/*==============================================================================================
    Alignment
==============================================================================================*/

// Align a pointer up to the next multiple of align (must be power of two).
ORB_INLINE void*
mem_align_ptr( void* ptr, usize align )
{
    usize addr = ( usize )ptr;
    addr       = ( addr + ( align - 1 ) ) & ~( align - 1 );
    return ( void* )addr;
}

// Align a size up to the next multiple of align (must be power of two).
ORB_INLINE usize
mem_align_size( usize size, usize align )
{
    return ( size + ( align - 1 ) ) & ~( align - 1 );
}

/*==============================================================================================
    Swap
==============================================================================================*/

// Swap two lvalues of the same type without a temp variable name collision.
#define mem_swap_t( a, b, T )      \
    do {                           \
        T _swap_tmp_ = ( a );      \
        ( a )        = ( b );      \
        ( b )        = _swap_tmp_; \
    }                              \
    while ( 0 )

/*============================================================================================*/
#endif // MEM_H