#ifndef MEM_H
#define MEM_H
/*==============================================================================================

    mem.h -- Memory operations (NO allocation).

        All functions are allocation-free. Allocators live in core/, not here.

==============================================================================================*/

// Copy n bytes from src to dst. Regions must NOT overlap.
void mem_copy( void* dst, const void* src, usize n );

// Copy n bytes, handling overlapping regions correctly.
void mem_move( void* dst, const void* src, usize n );

/*============================================================================================*/

// Fill n bytes at dst with value.
void mem_set( void* dst, u8 value, usize n );

// Zero n bytes at dst.
void mem_zero( void* dst, usize n );

// Zero a single typed value via pointer.
#define mem_zero_struct( ptr ) mem_zero( ( ptr ), sizeof( *( ptr ) ) )

// Zero an array of count elements (ptr must be a typed pointer).
#define mem_zero_array( ptr, count ) mem_zero( ( ptr ), sizeof( *( ptr ) ) * ( count ) )

/*============================================================================================*/

// Returns 1 if first n bytes of a and b are identical, 0 otherwise.
b32 mem_equal( const void* a, const void* b, usize n );

// Lexicographic compare; returns <0, 0, or >0 (same contract as memcmp).
int mem_compare( const void* a, const void* b, usize n );

/*============================================================================================*/

// Swap n bytes between a and b. Buffers must not overlap.
void mem_swap( void* a, void* b, usize n );

// Reverse n bytes in-place.
void mem_reverse( void* buf, usize n );

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

/*============================================================================================*/

// Swap two lvalues of the same type without a temp variable name collision.
#define mem_swap_t( a, b, T )      \
    do {                           \
        T _swap_tmp_ = ( a );      \
        ( a )        = ( b );      \
        ( b )        = _swap_tmp_; \
    }                              \
    while ( 0 )


/*============================================================================================*/
#endif MEM_H