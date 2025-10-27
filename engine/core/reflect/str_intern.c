/*==============================================================================================

    str_intern.c

==============================================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "orb.h"
#include "str_intern.h"

// Simple hash function (FNV-1a)
uint32_t
sid_hash( const char* s )
{
    uint32_t h = 2166136261u;
    while ( *s ) h = ( h ^ ( uint8_t )*s++ ) * 16777619u;
    return h;
}

/*============================================================================================*/

typedef struct          // Interned string entry in hash table
{
    uint32_t hash;      // precomputed string hash
    uint32_t offset;    // into arena
    uint16_t len;       // legnth (optional) -- we may remove this later

} intern_t;

typedef struct    // Simple arena allocator for string storage
{
    char*    base;
    uint32_t used;
    uint32_t capacity;

} arena_t;

typedef struct    // String interner with hash table and arena
{
    intern_t* entries;
    uint32_t  count;
    uint32_t  capacity;
    arena_t   arena;

} interner_t;

/*============================================================================================*/

// Initialize arena
static void
arena_init( arena_t* a, uint32_t cap )
{
    a->base     = malloc( cap );
    a->used     = 0;
    a->capacity = cap;
}

// Add string to arena, return offset
static uint32_t
arena_add( arena_t* a, const char* s, uint16_t len )
{
    if ( a->used + len + 1 > a->capacity )
        return UINT32_MAX;    // out of space

    uint32_t offset = a->used;
    memcpy( a->base + a->used, s, len );
    a->base[ a->used + len ] = 0;
    a->used += len + 1;
    return offset;
}

/*============================================================================================*/

// Interner parameters
#define INTERN_INIT_CAP   1024
#define INTERN_ARENA_SIZE ( 1024 * 4 )    // ( 1024 * 1024 )  1 MB initial arena
#define INTERN_MAX_LOAD   0.70f

// Initialize interner
static void
interner_init( interner_t* i )
{
    i->capacity = INTERN_INIT_CAP;
    i->count    = 0;
    i->entries  = calloc( i->capacity, sizeof( intern_t ) );
    arena_init( &i->arena, INTERN_ARENA_SIZE );
}

// Free interner resources
static void
interner_free( interner_t* i )
{
    free( i->entries );
    free( i->arena.base );
}

// Resize and rehash the table
static void
interner_resize( interner_t* interner, uint32_t new_cap )
{
    intern_t* old_entries  = interner->entries;    // save old table
    uint32_t  old_capacity = interner->capacity;

    interner->entries      = calloc( new_cap, sizeof( intern_t ) );
    interner->capacity     = new_cap;
    interner->count        = 0;

    for ( uint32_t i = 0; i < old_capacity; i++ )
    {
        // safety check (stop intellisense warnings)
        if ( interner == NULL || interner->entries == NULL )
            break;

        // skip empty slots
        if ( !old_entries[ i ].hash )
            continue;

        // debug string
        const char* s = interner->arena.base + old_entries[ i ].offset;
        UNUSED( s );

        // find the new slot by wrapping the NEW array size.
        uint32_t h   = old_entries[ i ].hash;
        uint32_t idx = h & ( interner->capacity - 1 );

        // linear probe to next free slot if we collided.
        while ( interner->entries[ idx ].hash ) idx = ( idx + 1 ) & ( interner->capacity - 1 );

        // insert into new slot
        interner->entries[ idx ] = old_entries[ i ];
        interner->count++;
    }

    free( old_entries );    // free old table
}

// Returns pointer to interned string (or adds it)

const char*
intern_string( interner_t* interner, const char* s )
{
    // compute hash and length
    uint16_t len = ( uint16_t )strlen( s );
    uint32_t h   = sid_hash( s );

    // resize if needed
    if ( ( float )interner->count / interner->capacity > INTERN_MAX_LOAD )
        interner_resize( interner, interner->capacity * 2 );

    // find slot in table for string
    uint32_t idx = h & ( interner->capacity - 1 );

    while ( 1 )
    {
        // check entry in hash table (if empty, add new)
        intern_t* e = &interner->entries[ idx ];
        if ( !e->hash )
        {
            // add new string to arena
            uint32_t off = arena_add( &interner->arena, s, len );
            if ( off == UINT32_MAX )
                return NULL;    // arena full

            e->hash   = h;
            e->offset = off;
            e->len    = len;

            // increment count and return pointer to interned string
            interner->count++;
            return interner->arena.base + off;
        }

        // check for existing string match
        if ( e->hash == h && e->len == len )
        {
            const char* es = interner->arena.base + e->offset;
            if ( memcmp( es, s, len ) == 0 )
                return es;    // found existing
        }
        idx = ( idx + 1 ) & ( interner->capacity - 1 );
    }
}

/*============================================================================================*/

int
intern_test( void )
{
    interner_t interner;
    interner_init( &interner );

    const char* a = intern_string( &interner, "r_fov" );
    const char* b = intern_string( &interner, "r_fov" );
    const char* c = intern_string( &interner, "r_gamma" );

    printf( "a == b ? %d\n", a == b );
    printf( "a == c ? %d\n", a == c );
    printf( "Arena used: %u bytes\n", interner.arena.used );

    interner_free( &interner );
    return 0;
}

/*============================================================================================*/

#define STRING_POOL_SIZE 4096
static char     g_string_pool[ STRING_POOL_SIZE ];
static uint32_t g_string_off = 0;

/*============================================================================================*/

void
str_intern_init()
{
    g_string_off = 0;
}

sid_t
str_intern( const char* s )
{
    uint32_t h = sid_hash( s );

    // Linear search for existing string (temprary simple implementation)
    uint32_t off = 0;
    while ( off < g_string_off )
    {
        const char* cur = g_string_pool + off;    // next string in pool
        if ( strcmp( cur, s ) == 0 )              // does it match our string
            return ( sid_t ){ h, off };           // yes -- return string id.
        off += ( uint32_t )strlen( cur ) + 1;     // incrememnet to next string
    }

    uint32_t len = ( uint32_t )strlen( s ) + 1;      // get length including null
    if ( g_string_off + len >= STRING_POOL_SIZE )    // check for available space in pool
    {
        len = 0;
        assert( 0 );
    }

    memcpy( g_string_pool + g_string_off, s, len );    // copy string into pool
    uint32_t new_off = g_string_off;                   // get current pool offset (id)
    g_string_off += len;                               // advance pool offset
    return ( sid_t ){ h, new_off };                    // return new string id
}

const char*
str_from_sid( sid_t sid )
{
    return g_string_pool + sid.off;    // return string from pool
}

const char*
str_from_off( uint32_t off )
{
    return g_string_pool + off;    // return string from pool
}

/*============================================================================================*/