/*==============================================================================================

    base/container.h -- Non-intrusive dynamic array (da_*).

    Works on ANY struct with these three fields, named exactly:

        typedef struct
        {
            T*  items;        // element pointer -- any type, this is what makes it generic
            i32 count;        // live element count
            i32 capacity;     // allocated element count
        } thing_array_t;

    No wrapper type, no generated struct, no void* + element-size bookkeeping. The struct is
    plain C -- inspectable in a debugger, embeddable in other structs, indexable with xs.items[i]
    with zero ceremony. da_* macros locate the element type via sizeof( *(xs).items ), so one
    set of macros drives every array type in the codebase. This is da_typedef() below.

    ALLOCATION:
        Routed through DA_REALLOC / DA_FREE, which default to realloc()/free(). Redefine both
        macros before including this header to swap in the engine allocator (arena, pool, etc)
        without touching a single call site. base cannot depend on the engine allocator yet
        (see mem.h: base performs NO allocation) -- this is the one deliberate exception, kept
        narrow and swappable for when core's allocator API lands.

    Usage:
        typedef struct { i32* items; i32 count; i32 capacity; } int_array_t;

        int_array_t xs = { 0 };
        da_append( xs, 10 );
        da_append( xs, 20 );
        for ( i32 i = 0; i < xs.count; i++ ) printf( "%d\n", xs.items[ i ] );
        da_free( xs );

==============================================================================================*/
#ifndef CONTAINER_H
#define CONTAINER_H

#include <stdlib.h>    // realloc / free -- see DA_REALLOC / DA_FREE override below

/*==============================================================================================
    Allocator hooks (override before #include "base/container.h" to redirect)
==============================================================================================*/

#ifndef DA_REALLOC
    #define DA_REALLOC( ptr, new_size ) realloc( ( ptr ), ( new_size ) )
#endif

#ifndef DA_FREE
    #define DA_FREE( ptr ) free( ( ptr ) )
#endif

/*==============================================================================================
    Declaration sugar (optional)

    da_typedef( T, name ) expands to a plain struct typedef -- not a hidden or opaque type.
    Hand-writing the three fields works exactly as well; this just saves the boilerplate.

        da_typedef( i32, int_array_t );
    is identical to
        typedef struct { i32* items; i32 count; i32 capacity; } int_array_t;
==============================================================================================*/

#define da_typedef( T, name ) \
    typedef struct            \
    {                         \
        T*  items;            \
        i32 count;            \
        i32 capacity;         \
    } name

/*==============================================================================================
    Growth
==============================================================================================*/

// Grow (xs).items to hold at least min_cap elements. No-op if capacity is already sufficient.
#define da_reserve( xs, min_cap )                                                                 \
    do                                                                                            \
    {                                                                                             \
        i32 _da_min_cap = ( i32 )( min_cap );                                                     \
        if ( ( xs ).capacity < _da_min_cap )                                                      \
        {                                                                                         \
            ( xs ).capacity = ( xs ).capacity ? ( xs ).capacity * 2 : 8;                          \
            if ( ( xs ).capacity < _da_min_cap )                                                  \
                ( xs ).capacity = _da_min_cap;                                                    \
            ( xs ).items = DA_REALLOC( ( xs ).items, sizeof( *( xs ).items ) * ( xs ).capacity ); \
            ORB_ASSERT( ( xs ).items != NULL );                                                   \
        }                                                                                         \
    }                                                                                             \
    while ( 0 )

/*==============================================================================================
    Insert
==============================================================================================*/

// Append x as the new last element, growing if needed.
#define da_append( xs, x )                      \
    do                                          \
    {                                           \
        da_reserve( ( xs ), ( xs ).count + 1 ); \
        ( xs ).items[ ( xs ).count++ ] = ( x ); \
    }                                           \
    while ( 0 )

// Append src_count elements from src_ptr (a plain T* or array), growing once for the whole run.
#define da_append_many( xs, src_ptr, src_count )                                               \
    do                                                                                         \
    {                                                                                          \
        i32 _da_n = ( i32 )( src_count );                                                      \
        da_reserve( ( xs ), ( xs ).count + _da_n );                                            \
        mem_copy( ( xs ).items + ( xs ).count, ( src_ptr ), sizeof( *( xs ).items ) * _da_n ); \
        ( xs ).count += _da_n;                                                                 \
    }                                                                                          \
    while ( 0 )

// Insert x at index i, shifting everything at/after i one slot to the right.
#define da_insert( xs, i, x )                                           \
    do                                                                  \
    {                                                                   \
        i32 _da_i = ( i32 )( i );                                       \
        ORB_ASSERT( _da_i >= 0 && _da_i <= ( xs ).count );              \
        da_reserve( ( xs ), ( xs ).count + 1 );                         \
        mem_move( ( xs ).items + _da_i + 1, ( xs ).items + _da_i,       \
                  sizeof( *( xs ).items ) * ( ( xs ).count - _da_i ) ); \
        ( xs ).items[ _da_i ] = ( x );                                  \
        ( xs ).count++;                                                 \
    }                                                                   \
    while ( 0 )

/*==============================================================================================
    Bounds-checked index helpers

    ORB_ASSERT expands to a do{}while(0) statement, so it cannot be folded into an expression
    via the comma operator -- these ORB_INLINE functions carry the assert instead, since a
    function call is itself a valid expression. Used by da_last / da_pop below.
==============================================================================================*/

ORB_INLINE i32
da_bounds_last_( i32 count )
{
    ORB_ASSERT( count > 0 );
    return count - 1;
}

ORB_INLINE i32
da_bounds_pop_( i32* count )
{
    ORB_ASSERT( *count > 0 );
    return --( *count );
}

/*==============================================================================================
    Remove
==============================================================================================*/

// O(1): remove index i by swapping in the last element. Does not preserve order.
#define da_remove_swap( xs, i )                                 \
    do                                                          \
    {                                                           \
        i32 _da_i = ( i32 )( i );                               \
        ORB_ASSERT( _da_i >= 0 && _da_i < ( xs ).count );       \
        ( xs ).items[ _da_i ] = ( xs ).items[ --( xs ).count ]; \
    }                                                           \
    while ( 0 )

// O(n): remove index i, shifting everything after it left one slot. Preserves order.
#define da_remove_ordered( xs, i )                                      \
    do                                                                  \
    {                                                                   \
        i32 _da_i = ( i32 )( i );                                       \
        ORB_ASSERT( _da_i >= 0 && _da_i < ( xs ).count );               \
        ( xs ).count--;                                                 \
        mem_move( ( xs ).items + _da_i, ( xs ).items + _da_i + 1,       \
                  sizeof( *( xs ).items ) * ( ( xs ).count - _da_i ) ); \
    }                                                                   \
    while ( 0 )

// Pop and yield the last element. Asserts count > 0.
#define da_pop( xs ) ( ( xs ).items[ da_bounds_pop_( &( xs ).count ) ] )

// Drop all elements. Capacity (and the backing allocation) is kept.
#define da_clear( xs ) ( ( xs ).count = 0 )

/*==============================================================================================
    Access
==============================================================================================*/

// Lvalue reference to the last element. Asserts count > 0.
#define da_last( xs ) ( ( xs ).items[ da_bounds_last_( ( xs ).count ) ] )

// True if the array holds no elements.
#define da_empty( xs ) ( ( xs ).count == 0 )

/*==============================================================================================
    Teardown
==============================================================================================*/

// Free the backing allocation and zero the struct back to its initial state.
#define da_free( xs )            \
    do                           \
    {                            \
        DA_FREE( ( xs ).items ); \
        ( xs ).items    = NULL;  \
        ( xs ).count    = 0;     \
        ( xs ).capacity = 0;     \
    }                            \
    while ( 0 )

/*============================================================================================*/
#endif    // CONTAINER_H
