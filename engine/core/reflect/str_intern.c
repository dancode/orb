/*==============================================================================================

    str_intern.c

    - Hash Table: A fixed-size hash table (g_intern_table) with linear probing.
    - String Storage: A fixed-size global char buffer (g_string_pool) acting as an arena.
    - String ID: A sid_t (which is a uint32_t) that is a direct offset into the g_string_pool.
    - Key Feature: Case-insensitive lookups, but case-preserving storage (the first-seen case becomes the "canonical" version).

==============================================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#include "orb.h"
#include "str_intern.h"

/*============================================================================================*/

uint32_t
next_pow2_u32( uint32_t v )
{
    // Round up to next power of 2

    if ( v == 0 )
        return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static bool
is_power_of_two( uint32_t v )
{
    /* Helper: check power-of-two */
    return v != 0 && ( ( v & ( v - 1 ) ) == 0 );
}

/*============================================================================================*/

// case sensitive hash function

// uint32_t
// hash_string( const char* s )
// {
//     // FNV-1a hash
//     uint32_t h = 2166136261u;
//     while ( *s ) h = ( h ^ ( uint8_t )*s++ ) * 16777619u;
//     return h;
// }
// 
// static uint32_t
// hash_string_len( const char* str, size_t len )
// {
//     // FNV-1a hash
//     uint32_t hash = 2166136261u;
//     for ( size_t i = 0; i < len; i++ )
//     {
//         hash ^= ( uint8_t )str[ i ];
//         hash *= 16777619u;
//     }
//     return hash;
// }

uint32_t
hash_string_ci( const char* s )
{
    // FNV-1a hash
    uint32_t h = 2166136261u;
    while ( *s ) h = ( h ^ ( uint8_t )tolower( ( unsigned char )*s++ ) ) * 16777619u;
    return h;
}

// Case-insensitive hash function
static uint32_t
hash_string_len_ci( const char* str, size_t len )
{
    uint32_t hash = 2166136261u;
    for ( size_t i = 0; i < len; i++ )
    {
        // Convert to lowercase during hash computation
        hash ^= ( uint8_t )tolower( ( unsigned char )str[ i ] );
        hash *= 16777619u;
    }
    return hash;
}

/*============================================================================================*/

uint32_t
sid_hash( const char* s )
{
    return hash_string_ci( s );
}

/*============================================================================================*/

// Case-insensitive string comparison
static bool
streq_ci( const char* a, const char* b, size_t len )
{
    for ( size_t i = 0; i < len; i++ )
    {
        if ( tolower( ( unsigned char )a[ i ] ) != tolower( ( unsigned char )b[ i ] ) )
        {
            return false;
        }
    }
    return true;
}

/*============================================================================================*/
/*
typedef enum
{
    SID_CAT_TYPE_NAME     = 0,    // "transform", "component"
    SID_CAT_PROPERTY_NAME = 1,    // "position", "rotation"
    SID_CAT_FUNCTION_NAME = 2,    // "update", "on_collision"
    SID_CAT_ENUM_VALUE    = 3,    // "IDLE", "RUNNING"
    SID_CAT_ASSET_PATH    = 4,    // "textures/brick.png"
    SID_CAT_TAG           = 5,    // "player", "enemy"
    SID_CAT_EVENT_NAME    = 6,    // "on_death", "on_spawn"
    SID_CAT_RESOURCE_ID   = 7,    // "ui_button_01"

} sid_category_t;

// Additional meta data to be added later for debugging or tracking

typedef struct intern_meta_s
{
    uint8_t  category;          // One of sid_category_t
    uint8_t  flags;             // static, dynamic, utf_8, preserve_case, etc.
    uint8_t  source_module;     // Which DLL/module defined this.
    uint8_t  reserved;          // padding
    uint16_t refcount;          // optional, for debug or unloading
    uint16_t source_file_id;    // Which file it came from.
    uint16_t source_line;       // Line number (for generated code)

} intern_meta_t;
*/

/*============================================================================================*/

// First string starts at offset 1 (after reserved byte)

typedef uint32_t sid_t;
#define SID_INVALID 0    // can't point to valid string (length 0)

static char     g_string_pool[ 16 * 1024 * 1024 ];    // 16 MB pool
static uint32_t g_string_pool_size = 0;

/*============================================================================================*/

static inline const char*
sid_cstr( sid_t sid )
{
    // Get string data (most common operation)
    return &g_string_pool[ sid ];    // skip length byte
}

static inline uint8_t
sid_length( sid_t sid )
{
    // Get length (less common, requires memory read)
    return ( uint8_t )g_string_pool[ sid - 1 ];    // read length byte
}

static inline bool
sid_equals( sid_t a, sid_t b )
{
    // Fast comparison (just integer equality!)
    return a == b;    // fastest possible - single comparison
}

bool
sid_is_canonical( sid_t sid, const char* str, size_t len )
{
    /* optional: check if provided string matches canonical case */

    if ( sid_length( sid ) != len )
        return false;
    return memcmp( sid_cstr( sid ), str, len ) == 0;    // Exact byte match
}

/*============================================================================================*/

#define INTERN_TABLE_SIZE 16384    // Power of 2

typedef struct    // Hash table entry: hash + sid (offset)
{
    uint32_t hash;    // Full 32-bit hash
    sid_t    sid;     // Offset into string pool (or SID_INVALID if empty)

} InternSlot;

static InternSlot g_intern_table[ INTERN_TABLE_SIZE ];    // 128 KB
static uint32_t   g_intern_count = 0;                     // Number of interned strings

/*============================================================================================*/

sid_t
sid_intern( const char* str, int32_t len )
{
    if ( len > 255 || len <= 0 )
    {
        assert( 0 );
        return SID_INVALID;    // Invalid length
    }

    uint32_t hash = hash_string_len_ci( str, len );
    uint32_t slot = hash & ( INTERN_TABLE_SIZE - 1 );

    // Linear probing
    while ( g_intern_table[ slot ].sid != SID_INVALID )
    {
        InternSlot* entry = &g_intern_table[ slot ];

        if ( entry->hash == hash )
        {
            sid_t   candidate     = entry->sid;
            uint8_t candidate_len = sid_length( candidate );

            // Case-INSENSITIVE comparison
            if ( candidate_len == len && streq_ci( sid_cstr( candidate ), str, len ) )
            {
                // Found existing (possibly different case!)
                // Return CANONICAL version (first interned)
                return candidate;
            }
        }

        slot = ( slot + 1 ) & ( INTERN_TABLE_SIZE - 1 );
    }

    // Not found - add NEW entry with EXACT case provided
    // Store with ORIGINAL case (becomes canonical)

    uint32_t header_offset = g_string_pool_size;
    uint32_t string_offset = header_offset + 1;    // sid points HERE

    assert( g_string_pool_size + 1 + len + 1 < sizeof( g_string_pool ) );

    // Write: [length][string data][null]
    g_string_pool[ header_offset ] = ( uint8_t )len;
    memcpy( &g_string_pool[ string_offset ], str, len );
    g_string_pool[ string_offset + len ] = '\0';
    g_string_pool_size += 1 + len + 1;

    // At the start of the "Not found - add NEW entry" section:
    assert( g_intern_count < INTERN_TABLE_SIZE && "String intern hash table is full!" );

    // Store sid pointing to STRING DATA (not length header)
    g_intern_table[ slot ].hash = hash;
    g_intern_table[ slot ].sid  = string_offset;
    g_intern_count++;

    return string_offset;
}

sid_t
sid_intern_cstr( const char* str )
{
    /* Convenience function for C strings */
    return sid_intern( str, ( int32_t )strlen( str ) );
}

/*============================================================================================*/

// Initialization
void
sid_init( void )
{
    // Set all slots to 0, which matches SID_INVALID
    memset( g_intern_table, 0, sizeof( g_intern_table ) );

    // First string must NOT be at offset 0 (reserved for SID_INVALID)
    // Offset 0 is reserved as invalid marker
    // It also defaults out string alignment to 2 (by starting at offset 1)

    g_intern_count     = 0;
    g_string_pool_size = 1;    // start pooling after length byte offset.
    g_string_pool[ 0 ] = 0;    // length 0 (invalid)
}

/*============================================================================================*/
/* Usage example */

int
intern_test( void )
{
    sid_t type_name = sid_intern_cstr( "transform" );

    /**************************************************************/
    // Intern strings (done once, typically at startup)
    /**************************************************************/

    sid_t transform = sid_intern_cstr( "transform" );
    sid_t component = sid_intern_cstr( "component" );
    sid_t position  = sid_intern_cstr( "position" );

    UNUSED( component );
    UNUSED( position );

    // Runtime usage (very fast - just integer operations)
    if ( type_name == transform )
    {
        // Process transform...
    }

    // Access string data when needed
    // printf( "Type: %s\n", sid_cstr( type_name ) );

    /**************************************************************/
    // Access length when needed (requires memory read, but uncommon)
    /**************************************************************/

    int32_t len = sid_length( type_name );
    char    buffer[ 256 ];
    snprintf( buffer, sizeof( buffer ), "%.*s_System", len, sid_cstr( type_name ) );

    /**************************************************************/
    // Hash is ALWAYS case-insensitive
    // Storage preserves FIRST interned case (canonical)
    /**************************************************************/

    sid_t transform1 = sid_intern_cstr( "Transform" );    // First! Canonical case
    sid_t transform2 = sid_intern_cstr( "TRANSFORM" );    // Returns same sid! Points to "Transform"
    sid_t transform3 = sid_intern_cstr( "transform" );    // Returns same sid! Points to "Transform"

    // All three are the SAME sid, all point to "Transform"
    assert( transform1 == transform2 );
    assert( transform2 == transform3 );

    // String pool only has ONE entry: "Transform"
    printf( "%s\n", sid_cstr( transform1 ) );    // "Transform"
    printf( "%s\n", sid_cstr( transform2 ) );    // "Transform" (not "TRANSFORM"!)
    printf( "%s\n", sid_cstr( transform3 ) );    // "Transform" (not "transform"!)

    return true;
}

/*============================================================================================*/