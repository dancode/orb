/*==============================================================================================

    memory.c

==============================================================================================*/

#include <stdio.h>     // printf
#include <stdlib.h>    // malloc, free
#include <string.h>    // memset
#include <stdint.h>    // int32_t, int64_t
#include <assert.h>    // assert
#include <stdarg.h>    //  va_list

#include "orb.h"
#include "core.h"
#include "core/sid/sid.h"

/*============================================================================================*/
/* enable/disable tracking at compile time */

#ifndef MEM_TAG_TRACK
#    define MEM_TAG_TRACK 1
#endif

/*==============================================================================================

    memory.c : data structures

==============================================================================================*/

#define MEM_MAX_TAGS 256

/* per-allocation heeder */

typedef struct mem_header_s
{
    int32_t tag;
    int32_t size;

} mem_header_t;

/* per-tag usage counter */

typedef struct mem_usage_s
{
    volatile int64_t current;    // current bytes allocated
    volatile int64_t peak;       // high water mark

} mem_usage_t;

/* global tracking data */

static mem_usage_t      g_memtag_usage[ MEM_MAX_TAGS ];    // our memory usage tracking
static sid_t            g_memtag_names[ MEM_MAX_TAGS ];    // mames for debug output
static volatile int64_t g_memtag_count  = MEMTAG_COUNT;    //  total number of tags allocated
static volatile int64_t g_total_current = 0;               // total memory all tags
static volatile int64_t g_total_peak    = 0;               // total peak memory all tags


/*==============================================================================================

    memory.c : helper functions

==============================================================================================*/

static void
mem_log( const char* fmt, ... )
{
    /* formatted log output for memory tracking debug */

    va_list ap;
    va_start( ap, fmt );
    vprintf( fmt, ap );
    va_end( ap );
}

/*==============================================================================================

    memory.c : platform atomics

==============================================================================================*/
#ifdef PLATFORM_WINDOWS

#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

static inline int64_t
atomic_exchange64( volatile int64_t* ptr, int64_t val )
{
    /* returns previous value */
    return _InterlockedExchange64( ptr, val );
}

static inline int64_t
atomic_add64( volatile int64_t* ptr, int64_t val )
{
    /* returns previous value */
    return _InterlockedExchangeAdd64( ptr, val );
}

static inline int64_t
atomic_increment64( volatile int64_t* ptr )
{
    /* returns incremented value */
    return _InterlockedIncrement64( ptr );
}

static inline int64_t
atomic_cas64( volatile int64_t* ptr, int64_t expected, int64_t newval )
{
    /* Atomic compare-exchange for peak update */
    return _InterlockedCompareExchange64( ptr, newval, expected );
}

static void
update_peak_strict( volatile int64_t* peak_ptr, int64_t new_value )
{
    /* Strict update of peak using CAS loop */

    for ( ;; )
    {
        long long old = *peak_ptr;
        if ( new_value <= old )
            return;
        /* try to swap old -> new_value */
        long long prev = atomic_cas64( peak_ptr, old, new_value );
        if ( prev == old )
            return; /* success */
        /* otherwise loop and retry */
    }
}

#endif
/*==============================================================================================

    memory.c : public stub api

==============================================================================================*/

void
mem_tag_init( void )
{
    /* initialize memory tagging system */
    
    memset( ( void* )g_memtag_usage, 0, sizeof( g_memtag_usage ) );
    memset( ( void* )g_memtag_names, 0, sizeof( g_memtag_names ) );

    atomic_exchange64( &g_memtag_count, MEMTAG_COUNT /* start after builtins */ );
    atomic_exchange64( &g_total_current, 0 );
    atomic_exchange64( &g_total_peak, 0 );

    // Initialize built-in tag names
    g_memtag_names[ MEMTAG_UNKNOWN ] = sid_intern_cstr( "unknown" );
    g_memtag_names[ MEMTAG_CORE ]    = sid_intern_cstr( "core" );
    g_memtag_names[ MEMTAG_ENGINE ]  = sid_intern_cstr( "engine" );

}

void
mem_tag_exit( void )
{
    /* shutdown memory tagging system */
    
    int64_t leaks = g_total_current;
    if ( leaks != 0 )
    {
        /* detect leaks by tag */
        printf( "\nMEMORY LEAK DETECTED: %lld bytes still allocated!\n\n", leaks );

        int64_t count = g_memtag_count;
        if ( count > MEM_MAX_TAGS )
             count = MEM_MAX_TAGS;

        for ( int64_t i = 0; i < count; ++i )
        {
            int64_t cur = g_memtag_usage[ i ].current;
            if ( cur != 0 )
            {
                const char* name = sid_cstr( g_memtag_names[ i ] );
                printf( "%-24s : %lld bytes leaked\n", name ? name : "(null)", cur );
            }
        }
    }
    else
    {
        printf( "No memory leaks detected.\n\n" );
    }
}

memtag_t
mem_tag_create( const char* name )
{
    /* create or reuse a tag -- returns index. */

    if ( !name )
        return MEMTAG_UNKNOWN;

    sid_t name_sid = sid_intern_cstr( name );

    /* rare operation: linear scan for existing name */
    for ( int i = 0; i < g_memtag_count; i++ )
    {
        sid_t tag_sid = g_memtag_names[ i ];
        if ( sid_equals( tag_sid, name_sid ) )
        {
            return ( memtag_t )i;    // return existing tag
        }
    }

    /* allocate new index via atomic increment; subtract 1 to get zero-based index */
    int64_t inc = atomic_increment64( &g_memtag_count );
    int64_t idx = inc - 1;
    if ( idx < 0 || idx >= MEM_MAX_TAGS )
    {
        assert( !"Exceeded maximum number of memory tags" );
        return MEMTAG_UNKNOWN;
    }

    g_memtag_names[ idx ] = name_sid;

    /* counters are already zeroed by init, but set explicitly for safety */
    atomic_exchange64( ( volatile int64_t* )&g_memtag_usage[ idx ].current, 0 );
    atomic_exchange64( ( volatile int64_t* )&g_memtag_usage[ idx ].peak, 0 );

    return ( memtag_t )idx;
}

void*
mem_tag_alloc( int32_t size, memtag_t tag )
{
    /* allocate memory with tag tracking */

    if ( !MEM_TAG_TRACK )
        return malloc( size );

    if ( tag < 0 || tag >= MEM_MAX_TAGS )
    {
        assert( 0 && "Invalid memory tag in mem_tag_alloc" );
        return NULL;
    }

    if ( size <= 0 )
    {
        assert( 0 && "Invalid size in mem_tag_alloc" );
        return NULL;
    }

    size_t   total = ( size_t )size + sizeof( mem_header_t );
    uint8_t* raw   = ( uint8_t* )malloc( total );
    if ( !raw )
        return NULL;

    mem_header_t* h = ( mem_header_t* )raw;
    h->tag          = tag;
    h->size         = size;

    /* update per-tag usage */
    int64_t prev      = atomic_add64( &g_memtag_usage[ tag ].current, ( int64_t )size );
    int64_t new_value = prev + ( int64_t )size;
    update_peak_strict( &g_memtag_usage[ tag ].peak, new_value );

    /* update total usage */
    int64_t prev_total = atomic_add64( &g_total_current, ( long long )size );
    int64_t new_total  = prev_total + ( long long )size;
    update_peak_strict( &g_total_peak, new_total );

    return raw + sizeof( mem_header_t );
}

void
mem_tag_free( void* ptr, int32_t size, memtag_t tag )
{
    /* free memory with tag tracking */

    if ( !ptr )
        return;

    if ( !MEM_TAG_TRACK )
    {
        free( ptr );
        return;
    }

    if ( tag < 0 || tag >= MEM_MAX_TAGS )
    {
        assert( 0 && "Invalid memory tag in mem_tag_free" );
        return;
    }

    if ( size < 0 )
    {
        assert( 0 && "Invalid size in mem_tag_free" );
        return;
    }

    uint8_t*      raw = ( uint8_t* )ptr - sizeof( mem_header_t );
    mem_header_t* h   = ( mem_header_t* )raw;

    if ( h->tag != tag || h->size != size )
    {
        /* use %d for signed fields */
        printf( "mem_tag_free mismatch! (hdr tag=%d size=%d) called with (tag=%d size=%d)\n", h->tag, h->size,
                ( int )tag, ( int )size );
        /* continue to free anyway */
    }

    /* atomic subtract: add negative value */
    atomic_add64( &g_memtag_usage[ tag ].current, -( int64_t )size );
    atomic_add64( &g_total_current, -( int64_t )size );

    free( raw );
}

static void
print_size_human( long long bytes )
{
    /* helper to print a human-friendly size */
    /* Human readable size printer */

    if ( bytes < 0 )
        bytes = 0;
    if ( bytes < 1024 )
    {
        printf( "B: %-6lld", ( int64_t )bytes );
    }
    else if ( bytes < ( 1024LL * 1024LL ) )
    {
        double kb = ( double )bytes / 1024.0;
        printf( "K: %-6.1f", kb );
    }
    else
    {
        double mb = ( double )bytes / ( 1024.0 * 1024.0 );
        printf( "M: %-6.2f", mb );
    }
}

void
mem_tag_dump( void )
{
    /* dump current memory usage by tag */

    mem_log( "\n----- Memory Usage -----\n\n" );
    int64_t count = g_memtag_count;
    if ( count > MEM_MAX_TAGS )
        count = MEM_MAX_TAGS;
    for ( int64_t i = 0; i < count; ++i )
    {
        const char* name = sid_cstr( g_memtag_names[ i ] );
        int64_t     cur  = g_memtag_usage[ i ].current;
        int64_t     peak = g_memtag_usage[ i ].peak;
        mem_log( "%-24s : current = ", name ? name : "(null)" );
        print_size_human( cur );
        mem_log( "  peak = " );
        print_size_human( peak );
        mem_log( "\n" );
    }
    mem_log( "\n%-24s : current = ", "TOTAL" );
    print_size_human( g_total_current );
    mem_log( "  peak = " );
    print_size_human( g_total_peak );
    mem_log( "\n\n" );
}

/*==============================================================================================

    memory.c : test tag tracking

==============================================================================================*/

void
mem_test( void )
{
    sid_init();

    printf( "=== MemTrack Test ===\n" );
    mem_tag_init();

    // Dynamically create runtime categories
    printf( "Creating tags...\n" );
    memtag_t TAG_RENDER  = mem_tag_create( "renderer" );
    memtag_t TAG_PHYSICS = mem_tag_create( "physics" );
    memtag_t TAG_AI      = mem_tag_create( "AI" );

    // Allocations (size passed just like your first API)
    void* a = mem_tag_alloc( 128, TAG_RENDER );
    void* b = mem_tag_alloc( 256, TAG_PHYSICS );
    void* c = mem_tag_alloc( 64, TAG_AI );

    mem_tag_dump();

    // Frees (size explicitly passed for validation)
    mem_tag_free( a, 128, TAG_RENDER );
    mem_tag_free( b, 256, TAG_PHYSICS );
    mem_tag_free( c, 64, TAG_AI );

    mem_tag_dump();
    mem_tag_exit();
    sid_exit();
}

/*============================================================================================*/