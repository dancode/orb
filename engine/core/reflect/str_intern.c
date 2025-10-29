/*==============================================================================================

    str_intern.c

    - Hash Table: Dynamic hash table with linear probing and automatic resizing
    - String Storage: Dynamic arena allocator that grows as needed
    - String ID: A sid_t (uint32_t) that is an offset into the string pool
    - Key Feature: Case-insensitive lookups, but case-preserving storage
    - Dynamic resizing, load factor management, debug metrics

    NOTE: Strings >255 are rejected -- all strings must be less than 256 characters.

==============================================================================================*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#include "orb.h"
#include "str_intern.h"
#include "base/standard.h"

/*==============================================================================================

    Hashing

==============================================================================================*/

static inline unsigned char
ascii_tolower( unsigned char c )
{
    return ( c >= 'A' && c <= 'Z' ) ? ( c + 32 ) : c;
}

uint32_t
sid_hash( const char* s )
{
    /* case-insensitive hash (FNV-1a) function */
    uint32_t h = 2166136261u;
    while ( *s ) h = ( h ^ ( uint8_t )ascii_tolower( ( unsigned char )*s++ ) ) * 16777619u;
    return h;
}

uint32_t
sid_hash_len( const char* str, size_t len )
{
    /* case-insensitive hash (FNV-1a) function */
    uint32_t hash = 2166136261u;
    for ( size_t i = 0; i < len; i++ )
    {
        hash ^= ( uint8_t )ascii_tolower( ( unsigned char )str[ i ] );
        hash *= 16777619u;
    }
    return hash;
}

/*==============================================================================================

    Configuration

==============================================================================================*/

#define DEFAULT_TABLE_SIZE ( 512 )                 // initial hash table size (power of 2) 16384
#define DEFAULT_ARENA_SIZE ( 4096 )                // initial size of string pool
#define MAX_ARENA_SIZE     ( 16 * 1024 * 1024 )    // max 16MB arena
#define MAX_STRING_LENGTH  255                     // 8-bit length limit
#define DEFAULT_MAX_LOAD   0.70f                   // rehash when 70% full

/*==============================================================================================

    Data Types

==============================================================================================*/

typedef struct intern_slot_s    // Hash table entry: hash + sid (offset)
{
    uint32_t hash;    // Full 32-bit hash
    sid_t    sid;     // Offset into string pool (or SID_INVALID if empty)

} intern_slot_t;

typedef struct string_arena_s
{
    char*    base;        // string storage arena
    uint32_t used;        // bytes used
    uint32_t capacity;    // total capacity

} string_arena_t;

typedef struct intern_state_s
{
    intern_slot_t* table;          // Hash table (dynamic)
    string_arena_t arena;          // String storage
    uint32_t       table_size;     // Current table size (power of 2)
    uint32_t       entry_count;    // Number of interned strings
    float          max_load;       // Load factor threshold

    uint32_t       total_lookups;       // statistics: total lookups
    uint32_t       total_probes;        // statics: total probes
    uint32_t       max_probe_length;    // statistics: max probe length
    uint32_t       rehash_count;        // statistics: number of rehashes

} intern_state_t;

static intern_state_t g_intern;

/*==============================================================================================

    Accessors + Utilities

==============================================================================================*/

const char*
sid_cstr( sid_t sid )
{
    if ( sid == SID_INVALID )
        return "";    // canonical empty string for invalid IDs

    assert( sid > 0 && sid < g_intern.arena.used );
    return &g_intern.arena.base[ sid ];
}

uint8_t
sid_length( sid_t sid )
{
    if ( sid == SID_INVALID )
        return 0;

    assert( sid > 0 && sid < g_intern.arena.used );
    return ( uint8_t )g_intern.arena.base[ sid - 1 ];
}

bool
sid_equals( sid_t a, sid_t b )
{
    return a == b;
}

bool
sid_is_canonical( sid_t sid, const char* str, size_t len )
{
    assert( sid != SID_INVALID );
    assert( sid > 0 && sid < g_intern.arena.used );

    if ( sid == SID_INVALID || str == NULL )
        return false;

    /* check if provided string matches canonical case */
    /* exact byte match that to internal representation */

    if ( sid_length( sid ) != len )
        return false;
    return memcmp( sid_cstr( sid ), str, len ) == 0;
}

/*==============================================================================================

    Internal : Arena Allocator

==============================================================================================*/

static bool
arena_init( string_arena_t* arena, uint32_t initial_capacity )
{
    /* Initialize arena with given initial capacity */

    arena->base = ( char* )malloc( initial_capacity );
    if ( !arena->base )
        return false;

    arena->capacity  = initial_capacity;    // total capacity
    arena->used      = 1;                   // reserve offset 0 for SID_INVALID
    arena->base[ 0 ] = 0;                   // length 0 (invalid marker)

    return true;
}

static void
arena_free( string_arena_t* arena )
{
    /* Free arena memory */

    free( arena->base );
    arena->base = NULL;
    arena->used = arena->capacity = 0;
}

static bool
arena_grow( string_arena_t* arena, uint32_t needed )
{
    /* Grow arena to ensure at least 'needed' bytes free */
    if ( arena->used + needed <= arena->capacity )
        return true; /* already enough space */

    /* Calculate new capacity (double until fits, capped at MAX_ARENA_SIZE) */

    uint32_t new_capacity = arena->capacity ? arena->capacity * 2 : DEFAULT_ARENA_SIZE;
    while ( arena->used + needed > new_capacity )
    {
        new_capacity *= 2;
        if ( new_capacity > MAX_ARENA_SIZE )
        {
            new_capacity = MAX_ARENA_SIZE;
            break;
        }
    }

    /* Overflowed the maximum arena size */
    if ( arena->used + needed > new_capacity )
        return false;

    /* Reallocate memory (or fail) */
    char* new_base = ( char* )realloc( arena->base, new_capacity );
    if ( !new_base )
        return false;

    arena->base     = new_base;
    arena->capacity = new_capacity;

    return true;
}

static sid_t
arena_add_string( string_arena_t* arena, const char* str, uint8_t len )
{
    /* Add string to arena, returns offset (or 0 on failure) */
    /* Format: [length_byte][string_data][null_terminator] */

    uint32_t needed = 1 + len + 1;    // length byte + data + null

    if ( !arena_grow( arena, needed ) )
        return SID_INVALID;

    /* Write string entry */

    uint32_t header_offset       = arena->used;
    uint32_t string_offset       = header_offset + 1;    // sid points to string data

    arena->base[ header_offset ] = len;
    memcpy( &arena->base[ string_offset ], str, len );
    arena->base[ string_offset + len ] = '\0';
    arena->used += needed;

    return string_offset;    // return offset to string data
}

/*==============================================================================================

    Internal : Hash Table

==============================================================================================*/

static bool
table_init( intern_slot_t** table, uint32_t size )
{
    /* Allocate and zero-initialize table */
    *table = ( intern_slot_t* )calloc( size, sizeof( intern_slot_t ) );
    return *table != NULL;
}

static void
table_free( intern_slot_t* table )
{
    /* Free table memory */
    free( table );
}

static bool
table_rehash( intern_state_t* state, uint32_t new_size )
{
    /* Rebuild table with new size, reinserting all entries */

    intern_slot_t* old_table = state->table;
    uint32_t       old_size  = state->table_size;

    /* Allocate new table */

    intern_slot_t* new_table;
    if ( !table_init( &new_table, new_size ) )
        return false;

    /* Reinsert all existing entries */

    for ( uint32_t i = 0; i < old_size; i++ )
    {
        if ( old_table[ i ].sid == SID_INVALID )
            continue;

        uint32_t hash = old_table[ i ].hash;
        uint32_t slot = hash & ( new_size - 1 );

        /* Linear probe to find empty slot */
        while ( new_table[ slot ].sid != SID_INVALID )
        {
            slot = ( slot + 1 ) & ( new_size - 1 );
        }

        new_table[ slot ] = old_table[ i ];
    }

    /* Free old table and update state */

    free( old_table );
    state->table      = new_table;
    state->table_size = new_size;
    state->rehash_count++;

    return true;
}

static bool
table_ensure_capacity( intern_state_t* state )
{
    /* Ensure table has capacity under load factor, rehash if needed */

    float load = ( float )( state->entry_count + 1 ) / ( float )state->table_size;
    if ( load <= state->max_load )
    {
        return true;    // <-- don't need new size
    }

    /* Need to rehash to larger table */

    uint32_t new_size = state->table_size * 2;
    return table_rehash( state, new_size );
}

/*==============================================================================================

    Public : Initialization + Shutdown

==============================================================================================*/

void
sid_init( void )
{
    memset( &g_intern, 0, sizeof( g_intern ) );

    /* Sanity: table size must be power-of-two */
    assert( ( DEFAULT_TABLE_SIZE & ( DEFAULT_TABLE_SIZE - 1 ) ) == 0 );

    /* initialize table and arena */

    if ( !table_init( &g_intern.table, DEFAULT_TABLE_SIZE ) )
    {
        assert( 0 && "Failed to initialize intern table" );
        return;
    }
    if ( !arena_init( &g_intern.arena, DEFAULT_ARENA_SIZE ) )
    {
        free( g_intern.table );
        assert( 0 && "Failed to initialize string arena" );
        return;
    }

    /* set initial parameters */

    g_intern.table_size  = DEFAULT_TABLE_SIZE;
    g_intern.entry_count = 0;
    g_intern.max_load    = DEFAULT_MAX_LOAD;

    /* clear statistics */

    g_intern.total_lookups    = 0;
    g_intern.total_probes     = 0;
    g_intern.max_probe_length = 0;
    g_intern.rehash_count     = 0;
}

void
sid_shutdown( void )
{
    /* free resources */

    table_free( g_intern.table );
    arena_free( &g_intern.arena );
    memset( &g_intern, 0, sizeof( g_intern ) );
}

/*==============================================================================================

    Public : String Interning

==============================================================================================*/

static bool
str_equal_insensitive( const char* a, const char* b, size_t len )
{
    /* case-insensitive string comparison with length */
    /* note: stirng a and b must have the same length */

    for ( size_t i = 0; i < len; i++ )
    {
        if ( tolower( ( unsigned char )a[ i ] ) != tolower( ( unsigned char )b[ i ] ) )
            return false;
    }
    return true;
}

sid_t
sid_intern( const char* str, int32_t len )
{
    /* Intern string with given length */

    if ( str == NULL )
    {
        assert( 0 && "NULL string passed to sid_intern" );
        return SID_INVALID;
    }
    if ( len <= 0 || len > MAX_STRING_LENGTH )
    {
        assert( 0 && "Invalid string length for interning" );
        return SID_INVALID;
    }
    if ( len == 0 )
    {
        // If you want to allow empty strings, uncomment the following lines:
        // return sid_intern_cstr("");
        assert( 0 && "Empty strings are not allowed (by current policy)" );
        return SID_INVALID;
    }

    uint32_t hash        = sid_hash_len( str, len );    // case-insensitive hash
    uint32_t mask        = g_intern.table_size - 1;     // table size mask
    uint32_t slot        = hash & mask;                 // initial slot
    uint32_t probe_count = 0;                           // probe statistics

    g_intern.total_lookups++;    // track total lookups

    /* linear probing - search for existing or empty slot */
    while ( g_intern.table[ slot ].sid != SID_INVALID )
    {
        probe_count++;
        intern_slot_t* entry = &g_intern.table[ slot ];

        if ( entry->hash == hash )
        {
            sid_t   candidate     = entry->sid;
            uint8_t candidate_len = sid_length( candidate );

            // Case-INSENSITIVE comparison
            if ( candidate_len == len && str_equal_insensitive( sid_cstr( candidate ), str, len ) )
            {
                g_intern.total_probes += probe_count;
                if ( probe_count > g_intern.max_probe_length )
                {
                    g_intern.max_probe_length = probe_count;
                }

                // Found existing (possibly different case!)
                // Return CANONICAL version (first interned)
                return candidate;    // <-- found existing entry
            }
        }
        slot = ( slot + 1 ) & mask;    // next slot
    }

    /* Not found - add NEW entry with EXACT case provided */

    /* Track probe statistics */
    g_intern.total_probes += probe_count;
    if ( probe_count > g_intern.max_probe_length )
    {
        g_intern.max_probe_length = probe_count;
    }

    /* Ensure capacity before adding -- may need to rehash */
    if ( !table_ensure_capacity( &g_intern ) )
    {
        assert( 0 && "Failed to grow hash table" );
        return SID_INVALID;
    }

    /* Recalculate slot after potential rehash */
    mask = g_intern.table_size - 1;
    slot = hash & mask;
    while ( g_intern.table[ slot ].sid != SID_INVALID )
    {
        slot = ( slot + 1 ) & mask;
    }

    /* Add string to arena */
    sid_t new_sid = arena_add_string( &g_intern.arena, str, ( uint8_t )len );
    if ( new_sid == SID_INVALID )
    {
        assert( 0 && "Arena allocation failed" );
        return SID_INVALID;
    }

    // Store in hash table
    g_intern.table[ slot ].hash = hash;
    g_intern.table[ slot ].sid  = new_sid;
    g_intern.entry_count++;

    return new_sid;
}

sid_t
sid_intern_cstr( const char* str )
{
    /* Convenience function for C strings */
    return sid_intern( str, ( int32_t )strlen( str ) );
}

/*==============================================================================================

    Public : Debug Statistics

==============================================================================================*/

typedef struct
{
    uint32_t entry_count;         // Number of unique strings
    uint32_t table_size;          // Hash table size
    uint32_t arena_used;          // Bytes used in arena
    uint32_t arena_capacity;      // Total arena capacity
    float    load_factor;         // Utilization of hash table
    uint32_t total_lookups;       // Total lookup operations
    uint32_t total_probes;        // Sum of all probe lengths
    float    avg_probe_length;    // Average probe length
    uint32_t max_probe_length;    // Longest probe sequence
    uint32_t rehash_count;        // Number of table rehashes
    uint32_t collisions;          // Entries that had to probe

} sid_stats_t;

static sid_stats_t
sid_get_stats( void )
{
    sid_stats_t stats    = { 0 };

    stats.entry_count    = g_intern.entry_count;
    stats.table_size     = g_intern.table_size;
    stats.arena_used     = g_intern.arena.used;
    stats.arena_capacity = g_intern.arena.capacity;
    stats.load_factor = g_intern.table_size > 0 ? ( float )g_intern.entry_count / ( float )g_intern.table_size : 0.0f;

    stats.total_lookups    = g_intern.total_lookups;
    stats.total_probes     = g_intern.total_probes;
    stats.max_probe_length = g_intern.max_probe_length;
    stats.rehash_count     = g_intern.rehash_count;

    stats.avg_probe_length =
        g_intern.total_lookups > 0 ? ( float )g_intern.total_probes / ( float )g_intern.total_lookups : 0.0f;

    // Calculate collisions (entries that didn't land in ideal slot)
    for ( uint32_t i = 0; i < g_intern.table_size; i++ )
    {
        if ( g_intern.table[ i ].sid == SID_INVALID )
            continue;

        uint32_t ideal_slot = g_intern.table[ i ].hash & ( g_intern.table_size - 1 );
        if ( ideal_slot != i )
            stats.collisions++;
    }

    return stats;
}

void
sid_print_stats( void* fp )
{
    /* Get stats */

    sid_stats_t s = sid_get_stats();

    /* Print stats */

    FILE* f = ( FILE* )fp;
    fprintf( f, "=== String Interner Statistics ===\n" );
    fprintf( f, "Entries:           %u\n", s.entry_count );
    fprintf( f, "Hash table:        %u buckets\n", s.table_size );
    fprintf( f, "Load factor:       %.2f%%\n", s.load_factor * 100.0f );
    fprintf( f, "Arena:             %u / %u bytes (%.1f%% used)\n", s.arena_used, s.arena_capacity,
             s.arena_capacity > 0 ? ( s.arena_used * 100.0f / s.arena_capacity ) : 0.0f );
    fprintf( f, "Total lookups:     %u\n", s.total_lookups );
    fprintf( f, "Total probes:      %u\n", s.total_probes );
    fprintf( f, "Collisions:        %u (%.1f%%)\n", s.collisions,
             s.entry_count > 0 ? ( s.collisions * 100.0f / s.entry_count ) : 0.0f );
    fprintf( f, "Max probe length:  %u\n", s.max_probe_length );
    fprintf( f, "Avg probe length:  %.2f\n", s.avg_probe_length );
    fprintf( f, "Rehash count:      %u\n", s.rehash_count );
    fprintf( f, "===================================\n" );
}

void
sid_reset_stats( void )
{
    g_intern.total_lookups    = 0;
    g_intern.total_probes     = 0;
    g_intern.max_probe_length = 0;

    // note: Don't reset rehash_count as it's cumulative
}

/*==============================================================================================

    Public : Usage Example

==============================================================================================*/
int
intern_test( void )
{
    sid_init();

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
    printf( "Type: %s\n", sid_cstr( type_name ) );

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

    // Print statistics
    printf( "\n" );
    sid_print_stats( stdout );

    /**************************************************************/
    sid_shutdown();

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