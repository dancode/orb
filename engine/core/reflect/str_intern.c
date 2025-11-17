/*==============================================================================================

    str_intern.c

    - Hash Table: Dynamic hash table with linear probing and automatic resizing
    - String Storage: Dynamic arena allocator that grows as needed
    - String ID: A sid_t (uint32_t) that is an offset into the string pool
    - Key Feature: Case-insensitive lookups, but case-preserving storage
    - Dynamic resizing, load factor management, debug metrics

    NOTE: Strings > 255 are rejected -- all strings must be less than 256 characters.

    - NOTE: this is not currenttly thread-safe.

==============================================================================================*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#include "orb.h"
#include "str_intern.h"
#include "../core.h"

// clang-format off
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

    Intern : Configuration

==============================================================================================*/

#define DEFAULT_TABLE_SIZE ( 512 )                  // initial entry hash table size (power of 2)
#define DEFAULT_MAX_LOAD   0.70f                    // rehash when 70% full or 50% for perf

#define DEFAULT_ARENA_SIZE ( 4096 )                 // initial size of string pool in bytes.
#define MAX_ARENA_SIZE     ( 16 * 1024 * 1024 )     // max 16MB arena
#define MAX_STRING_LENGTH  255                      // string length limit

/*==============================================================================================

    Intern : Data Types

==============================================================================================*/
/*
typedef struct intern_slot_s            // hash table entry: hash + sid (offset)
{
    uint32_t        hash;               // full 32-bit hash
    sid_t           sid;                // offset into string pool (or SID_INVALID if empty)

} intern_slot_t;

typedef struct string_arena_s
{
    char*           base;               // string storage arena
    uint32_t        used;               // bytes used
    uint32_t        capacity;           // total capacity

} string_arena_t;

typedef struct intern_state_s
{
    intern_slot_t*  table;              // hash table (dynamically sized)
    string_arena_t  arena;              // string storage (dynamically sized)
    uint32_t        table_size;         // entry table capacity (power of 2)
    uint32_t        entry_count;        // number of entires (interned strings) used.
    float           max_load;           // Load factor threshold (e.g., 0.7 for 70%)

    uint32_t        total_lookups;      // statistics: total lookups (number of sid_intern calls)
    uint32_t        total_probes;       // statistics: total probes (sum of all probe lengths)
    uint32_t        max_probe_length;   // statistics: max probe length (longest probe sequence)
    uint32_t        rehash_count;       // statistics: number of rehashes (resizes)

} intern_state_t;
*/

intern_state_t g_intern;


/*==============================================================================================

    Internal : String Arena Allocator

==============================================================================================*/

static bool
arena_init( string_arena_t* arena, uint32_t initial_capacity )
{
    /* initialize arena with given initial capacity */

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
    /* free arena memory */

    free( arena->base );
    arena->base = NULL;
    arena->used = arena->capacity = 0;
}

static bool
arena_grow( string_arena_t* arena, uint32_t needed )
{
    /* grow arena to ensure at least 'needed' bytes free */
    /* this function should never fail unless we exceed MAX_ARENA_SIZE */

    if ( arena->used + needed <= arena->capacity )
        return true; /* enough space available */

    /* calculate new capacity (double until fit) */
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

    /* only occurs if we overflowed the maximum arena size */
    if ( arena->used + needed > new_capacity ) {
        assert( 0 && "String arena exceeded maximum size" && MAX_ARENA_SIZE );
        return false;
    }

    /* reallocate memory (or fail) */
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
    /* add string to arena, returns offset (or 0 on failure) */
    /* format: [ length_byte ][ string_data ][ null_terminator ] */

    uint32_t needed = 1 + len + 1;    // length byte + data + null

    if ( !arena_grow( arena, needed ) )
        return SID_INVALID;

    /* write string entry */

    uint32_t header_offset       = arena->used;
    uint32_t string_offset       = header_offset + 1;    // sid points to string data

    arena->base[ header_offset ] = len;
    memcpy( &arena->base[ string_offset ], str, len );
    arena->base[ string_offset + len ] = '\0';
    arena->used += needed;

    return ( sid_t ){ string_offset };    // return offset to string data
}

/*==============================================================================================

    Internal : Entry Hash Table

==============================================================================================*/

static bool
table_init( intern_slot_t** table, uint32_t size )
{
    /* allocate and zero-initialize table */
    /* called on intern table init and table rehash */
    *table = ( intern_slot_t* )calloc( size, sizeof( intern_slot_t ) );
    return *table != NULL;
}

static void
table_free( intern_slot_t* table )
{
    /* free table memory */
    free( table );
}

static bool
table_rehash( intern_state_t* state, uint32_t new_size )
{
    /* rebuild table with new size, reinserting all entries */

    intern_slot_t* old_table = state->table;
    uint32_t       old_size  = state->table_size;

    /* allocate new table */
    intern_slot_t* new_table;
    if ( !table_init( &new_table, new_size ) )
        return false;

    uint32_t mask = new_size - 1;

    /* reinsert all existing entries */
    for ( uint32_t i = 0; i < old_size; i++ )
    {
        if ( sid_equals( old_table[ i ].sid, SID_INVALID ))
            continue;

        uint32_t hash = old_table[ i ].hash;
        uint32_t slot = hash & mask;

        /* Linear probe insertion during rehash */
        while ( !sid_equals( new_table[ slot ].sid, SID_INVALID ))
        {
            slot = ( slot + 1 ) & mask;
        }
        new_table[ slot ] = old_table[ i ];
    }

    /* free old table and update state */
    free( old_table );
    state->table      = new_table;
    state->table_size = new_size;
    state->rehash_count++;

    return true;
}

static bool
table_ensure_capacity( intern_state_t* state )
{
    /* ensure table has capacity under load factor, rehash if needed */
    /* entry count is increased after the call so (increment) it before */

    float load = ( float )( state->entry_count + 1 ) / ( float )state->table_size;
    if ( load <= state->max_load )
    {
        return true;    // <-- we are under load
    }

    /* we need to rehash to a larger table */

    uint32_t new_size = state->table_size * 2;
    return table_rehash( state, new_size );
}

/*==============================================================================================

    SID : Usage

==============================================================================================*/

const char*
sid_cstr( sid_t sid )
{
    // canonical empty string for invalid IDs
    if ( sid_equals( sid, SID_INVALID ))
        return "";

    assert( sid.off > 0 && sid.off < g_intern.arena.used );
    return &g_intern.arena.base[ sid.off ];
}

uint8_t
sid_length( sid_t sid )
{
    // canonical zero legnth for invalid IDs
    if ( sid_equals( sid, SID_INVALID ))
        return 0;

    assert( sid.off > 0 && sid.off < g_intern.arena.used );
    return ( uint8_t )g_intern.arena.base[ sid.off - 1 ];
}

bool inline 
sid_equals( sid_t a, sid_t b )
{
    // super faster comparison by sid value (offset)
    return a.off == b.off;
}

bool
sid_is_canonical( sid_t sid, const char* str, size_t len )
{
    assert( !sid_equals( sid, SID_INVALID ));
    assert( sid.off > 0 && sid.off < g_intern.arena.used );

    // we expect valid inputs
    if ( sid_equals( sid, SID_INVALID ) || str == NULL )
        return false;

    // check if provided string matches the canonical case
    // that is, an exact byte match that to internal representation

    if ( sid_length( sid ) != len )
        return false;

    return memcmp( sid_cstr( sid ), str, len ) == 0;
}

uint32_t
sid_get_hash( sid_t sid )
{
    assert( !sid_equals( sid, SID_INVALID ) );
    assert( sid.off > 0 && sid.off < g_intern.arena.used );

    // we expect valid inputs
    if ( sid_equals( sid, SID_INVALID ))
        return 0;

    // retrieve hash from intern table entry
    return sid_hash( sid_cstr( sid ));
}

/*==============================================================================================

    SID : Initialization + Shutdown

==============================================================================================*/

void
sid_init( void )
{
    /* Initialize global intern state */
    memset( &g_intern, 0, sizeof( g_intern ) );

    /* Sanity: table size must be power-of-two */
    assert( ( DEFAULT_TABLE_SIZE & ( DEFAULT_TABLE_SIZE - 1 ) ) == 0 );

    /* initialize table and arena */
    if ( table_init( &g_intern.table, DEFAULT_TABLE_SIZE ) == false )
    {
        assert( 0 && "Failed to initialize intern table" );
        return;
    }
    if ( arena_init( &g_intern.arena, DEFAULT_ARENA_SIZE ) == false )
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

    SID : Interning

==============================================================================================*/

static bool
str_equal_insensitive( const char* a, const char* b, size_t len )
{
    /* case-insensitive string comparison with length */
    /* note: stirng a and b must have the same length */

    for ( size_t i = 0; i < len; i++ )
    {
        if ( ascii_tolower( ( unsigned char )a[ i ] ) !=
             ascii_tolower( ( unsigned char )b[ i ] ) )
            return false;
    }
    return true;
}

static inline uint32_t
probe_distance( uint32_t hash, uint32_t slot, uint32_t mask )
{    
    uint32_t ideal = hash & mask;       // how far from ideal position?
    return ( slot - ideal ) & mask;     // wraps around correctly
}

sid_t
sid_intern( const char* str, int32_t len )
{
    /* intern string with given length */
    /* Note: we always return the CANONICAL version (the first interned) */

    if ( str == NULL )
    {
        assert( 0 && "NULL string passed to sid_intern" );
        return SID_INVALID;
    }
    if ( len < 0 || len > MAX_STRING_LENGTH )
    {
        assert( 0 && "Invalid string length for interning" );
        return SID_INVALID;
    }
    if ( len == 0 )
    {
        // handle empty string with generic singleton
        return sid_intern_cstr("");
    }

    uint32_t hash        = sid_hash_len( str, len );    // case-insensitive hash
    uint32_t mask        = g_intern.table_size - 1;     // table size mask
    uint32_t slot        = hash & mask;                 // initial slot
    uint32_t probe_count = 0;                           // probe statistics

    g_intern.total_lookups++;    // track total lookups

    /* linear probing - search for existing or empty slot */
    while ( !sid_equals( g_intern.table[ slot ].sid, SID_INVALID ))
    {
        probe_count++;
        intern_slot_t* entry = &g_intern.table[ slot ]; 

        if ( entry->hash == hash )
        {
            sid_t   candidate     = entry->sid;
            uint8_t candidate_len = sid_length( candidate );

            /* case-INSENSITIVE comparison */
            if ( candidate_len == len && str_equal_insensitive( sid_cstr( candidate ), str, len ) )
            {
                g_intern.total_probes += probe_count;
                if ( probe_count > g_intern.max_probe_length ) {
                     g_intern.max_probe_length = probe_count;
                }
                return candidate;    // <-- found existing entry
            }
        }
        slot = ( slot + 1 ) & mask;    // try next slot
    }

    /* entry not found - add a NEW entry with EXACT case provided */

    /* update probe statistics */
    g_intern.total_probes += probe_count;
    if ( probe_count > g_intern.max_probe_length ) {
        g_intern.max_probe_length = probe_count;
    }

    /* ensure capacity before adding -- may need to rehash */
    if ( !table_ensure_capacity( &g_intern ) )
    {
        assert( 0 && "failed to grow hash table" );
        return SID_INVALID;
    }

    /* add string to arena first */
    sid_t new_sid = arena_add_string( &g_intern.arena, str, ( uint8_t )len );
    if ( sid_equals( new_sid, SID_INVALID ))
    {
        assert( 0 && "string arena allocation failed" );
        return SID_INVALID;
    }

    /* recalculate slot after potential rehash */
    mask = g_intern.table_size - 1;
    slot = hash & mask;
       
    /* linear probe to find insertion slot */
    while ( !sid_equals( g_intern.table[ slot ].sid, SID_INVALID ))
    {
        slot = ( slot + 1 ) & mask;
    }    
    /* store entry in the hash table */
    g_intern.table[ slot ].hash = hash;
    g_intern.table[ slot ].sid  = new_sid;
    
    g_intern.entry_count++;
    return new_sid;
}

sid_t
sid_intern_cstr( const char* str )
{
    if ( str == NULL ) return SID_INVALID;

    /* convenience function for C strings */
    return sid_intern( str, ( int32_t )strlen( str ) );
}

/*==============================================================================================

    SID : Debug Statistic Tracking

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
    /* gather current statistics */

    sid_stats_t stats    = { 0 };

    stats.entry_count       = g_intern.entry_count;
    stats.table_size        = g_intern.table_size;
    stats.arena_used        = g_intern.arena.used;
    stats.arena_capacity    = g_intern.arena.capacity;
    stats.load_factor       = g_intern.table_size > 0 ? 
                            ( float )g_intern.entry_count / ( float )g_intern.table_size : 0.0f;

    stats.total_lookups     = g_intern.total_lookups;
    stats.total_probes      = g_intern.total_probes;
    stats.max_probe_length  = g_intern.max_probe_length;
    stats.rehash_count      = g_intern.rehash_count;

    stats.avg_probe_length  = g_intern.total_lookups > 0 ? 
                            ( float )g_intern.total_probes / ( float )g_intern.total_lookups : 0.0f;

    // Calculate collisions (entries that didn't land in ideal slot)
    for ( uint32_t i = 0; i < g_intern.table_size; i++ )
    {
        if ( sid_equals( g_intern.table[ i ].sid, SID_INVALID ))
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
    /* print current statistics to given file pointer */

    sid_stats_t s = sid_get_stats();

    /* print stats */

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
    /* reset period statistics counters */
    /* typically not called except for testing */

    g_intern.total_lookups    = 0;
    g_intern.total_probes     = 0;
    g_intern.max_probe_length = 0;

    /* note: we don't reset rehash_count, it is cumulative */
}

// clang-format on

/*==============================================================================================

    SID : Test linear probing performance.

==============================================================================================*/
void
hash_perf_test()
{
    /**************************************************************/
    // Test: Probing yersy
    /**************************************************************/
    {
        printf( "\n========================================\n" );
        printf( "Linear Probe Performance Test\n" );
        printf( "========================================\n\n" );

        // Phase 1: Insert strings with deliberately bad hash distribution
        // to create clustering (worst case for linear probing)
        printf( "Phase 1: Inserting 1000 strings...\n" );

        char  key[ 64 ];
        sid_t test_sids[ 1000 ];

        // Mix of different patterns to stress the hash table
        for ( int i = 0; i < 1000; i++ )
        {
            if ( i < 300 )
            {
                // Pattern 1: Sequential keys (often cluster)
                snprintf( key, sizeof( key ), "key_%04d", i );
            }
            else if ( i < 600 )
            {
                // Pattern 2: Similar prefixes (hash collisions likely)
                snprintf( key, sizeof( key ), "transform_component_%d", i );
            }
            else if ( i < 800 )
            {
                // Pattern 3: Common game entity names
                const char* prefixes[] = { "player", "enemy", "item", "projectile", "effect" };
                snprintf( key, sizeof( key ), "%s_%d", prefixes[ i % 5 ], i );
            }
            else
            {
                // Pattern 4: Asset paths (dots and slashes)
                snprintf( key, sizeof( key ), "assets/textures/level_%d.png", i );
            }

            test_sids[ i ] = sid_intern_cstr( key );
            assert( !sid_equals( test_sids[ i ], SID_INVALID ));
        }

        printf( "Phase 1 complete.\n\n" );
        printf( "Statistics after insertion:\n" );
        sid_print_stats( stdout );

        // Phase 2: Perform many lookups
        printf( "\nPhase 2: Performing 10,000 lookups (re-interning existing strings)...\n" );

        sid_reset_stats();    // Reset to measure lookup-only performance

        // Lookup in different patterns
        for ( int round = 0; round < 10; round++ )
        {
            // Sequential access
            for ( int i = 0; i < 300; i++ )
            {
                snprintf( key, sizeof( key ), "key_%04d", i );
                sid_t s = sid_intern_cstr( key );
                assert( sid_equals( s, test_sids[ i ] ));
            }

            // Random-ish access (worst case - jumps around memory)
            for ( int i = 300; i < 600; i++ )
            {
                int idx = 300 + ( ( i * 37 ) % 300 );    // pseudo-random
                snprintf( key, sizeof( key ), "transform_component_%d", idx );
                sid_t s = sid_intern_cstr( key );
                assert( !sid_equals( s, SID_INVALID ));
            }

            // Repeated hot strings (cache-friendly)
            for ( int i = 0; i < 100; i++ )
            {
                snprintf( key, sizeof( key ), "player_%d", 600 + ( i % 10 ) );
                sid_t s = sid_intern_cstr( key );
                assert( !sid_equals( s, SID_INVALID ));
            }

            // Different case variations (tests case-insensitive lookup)
            for ( int i = 0; i < 100; i++ )
            {
                snprintf( key, sizeof( key ), "KEY_%04d", i );    // uppercase
                sid_t s = sid_intern_cstr( key );
                assert( sid_equals( s, test_sids[ i ] ));    // should match lowercase version
            }
        }

        printf( "Phase 2 complete.\n\n" );
        printf( "Statistics after 10,000 lookups:\n" );
        sid_print_stats( stdout );

        // Phase 3: Stress test - fill to high load factor
        printf( "\nPhase 3: Stress test - filling to ~65%% load...\n" );

        int additional = 0;
        // Current load is ~1000/1024 = 97% (after one resize from 512)
        // We're likely at 2048 table size, so add more to stay under max load
        while ( additional < 500 )
        {
            snprintf( key, sizeof( key ), "stress_test_key_%d", 1000 + additional );
            sid_t s = sid_intern_cstr( key );
            if ( sid_equals( s, SID_INVALID ))
                break;
            additional++;
        }

        printf( "Added %d additional strings.\n\n", additional );
        printf( "Final statistics:\n" );
        sid_print_stats( stdout );
    }
}

// extern core_api_t*       g_api;
// extern core_debug_api_t* g_debug_api;


/*==============================================================================================

    SID : Usage Example

==============================================================================================*/
int
intern_test( void )
{
    /**************************************************************/
    // Test: Init and Shutdown
    /**************************************************************/
    {
        sid_init();
        sid_shutdown();
    }
    /**************************************************************/
    // Test: Case Inensitive and Canonical
    /**************************************************************/
    {
        sid_init();

        const char* s    = "Transform";
        const char* sl   = "transform";
        const char* su   = "TRANSFORM";
        const int   len  = 9;


        sid_t       sid1 = sid_intern_cstr( s );
        sid_t       sid2 = sid_intern_cstr( sl );
        sid_t       sid3 = sid_intern( su, len );

        core_debug_api_t* d = core_debug_get_api();
        const char*       teststirng = ( char* )( d->intern_arena->base + sid1.off ); 

        UNUSED( teststirng );

        assert( !sid_equals( sid1, SID_INVALID ));
        assert(  sid_equals( sid1, sid2 ));
        assert(  sid_equals( sid2, sid3 ));

        assert( strcmp( sid_cstr( sid1 ), s ) == 0 );
        assert( sid_is_canonical( sid1, s, ( size_t )len ) );
        assert( !sid_is_canonical( sid1, sl, ( size_t )len ) );
        assert( !sid_is_canonical( sid1, su, ( size_t )len ) );

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Mixed Case In Middle of String
    /**************************************************************/
    {
        sid_init();

        sid_t s1 = sid_intern_cstr( "MixedCaseString" );
        sid_t s2 = sid_intern_cstr( "mixedcasestring" );
        sid_t s3 = sid_intern_cstr( "MIXEDCASESTRING" );

        assert( sid_equals( s1, s2 ) && sid_equals( s2, s3 ));
        assert( strcmp( sid_cstr( s1 ), "MixedCaseString" ) == 0 );

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Special Characters
    /**************************************************************/
    {
        sid_init();

        const char* specials[] = { "test_123",   "test-with-dashes", "test.with.dots", "test/path",
                                   "test\\path", "test:colon",       "test@symbol",    "test#hash" };

        for ( size_t i = 0; i < sizeof( specials ) / sizeof( specials[ 0 ] ); i++ )
        {
            sid_t s = sid_intern_cstr( specials[ i ] );
            assert( !sid_equals( s, SID_INVALID ));
            assert( strcmp( sid_cstr( s ), specials[ i ] ) == 0 );
        }

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Single Character Strings
    /**************************************************************/
    {
        sid_init();

        sid_t s1 = sid_intern_cstr( "A" );
        sid_t s2 = sid_intern_cstr( "a" );

        assert( sid_equals( s1, s2 ));
        assert( strcmp( sid_cstr( s1 ), "A" ) == 0 );
        assert( sid_length( s1 ) == 1 );

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Accessors and Equals
    /**************************************************************/
    {
        sid_init();

        sid_t t = sid_intern_cstr( "type" );
        sid_t p = sid_intern_cstr( "position" );

        assert( sid_equals( t, t ) );
        assert( !sid_equals( t, p ) );

        assert( sid_length( t ) == ( uint8_t )strlen( "type" ) );
        assert( sid_length( p ) == ( uint8_t )strlen( "position" ) );

        const char* invalid = sid_cstr( SID_INVALID );
        assert( invalid != NULL );
        assert( invalid[ 0 ] == '\0' );

        sid_shutdown();
    }
    /**************************************************************/
    // Test: String Boundary Length
    /**************************************************************/
    {
        sid_init();

        char buf[ 256 ];
        memset( buf, 'a', 255 );
        buf[ 255 ] = '\0';

        sid_t s    = sid_intern( buf, 255 );
        assert( !sid_equals( s, SID_INVALID ));
        assert( sid_length( s ) == 255 );

        const char* stored = sid_cstr( s );
        assert( stored != NULL );
        assert( memcmp( stored, buf, 255 ) == 0 );

        // Note: Do NOT test len 0 or >255 here, as implementation asserts on invalid lengths.

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Hash Functions
    /**************************************************************/
    {
        sid_init();

        const char* a  = "AbC";
        const char* b  = "aBc";
        uint32_t    h1 = sid_hash( a );
        uint32_t    h2 = sid_hash( b );
        assert( h1 == h2 );

        const char* samples[] = { "transform", "Component", "POSITION", "Key_0123" };
        for ( size_t i = 0; i < sizeof( samples ) / sizeof( samples[ 0 ] ); ++i )
        {
            const char* s  = samples[ i ];
            uint32_t    hA = sid_hash( s );
            uint32_t    hB = sid_hash_len( s, strlen( s ) );
            assert( hA == hB );
        }

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Rehashing and Arena Grow
    /**************************************************************/
    {
        sid_init();

        enum
        {
            COUNT = 600
        };    // > 0.7 * 512 to trigger at least one rehash

        char  key[ 32 ];
        sid_t samples[ 5 ]    = { 0 };
        int   sample_idx[ 5 ] = { 0, 1, 123, 357, 599 };

        for ( int i = 0; i < COUNT; ++i )
        {
            snprintf( key, sizeof( key ), "key_%04d", i );
            sid_t s = sid_intern_cstr( key );
            assert( !sid_equals( s, SID_INVALID ));

            // capture a few sample SIDs to validate stability across growth
            for ( size_t k = 0; k < sizeof( sample_idx ) / sizeof( sample_idx[ 0 ] ); ++k )
            {
                if ( i == sample_idx[ k ] )
                    samples[ k ] = s;
            }
        }

        // Re-intern with different cases to verify case-insensitive lookup and canonical storage
        for ( size_t k = 0; k < sizeof( sample_idx ) / sizeof( sample_idx[ 0 ] ); ++k )
        {
            int idx = sample_idx[ k ];
            snprintf( key, sizeof( key ), "KEY_%04d", idx );    // different case
            sid_t again = sid_intern_cstr( key );
            assert( sid_equals( again, samples[ k ] ));

            // verify canonical matches first-inserted lowercase "key_xxxx"
            char canonical[ 32 ];
            snprintf( canonical, sizeof( canonical ), "key_%04d", idx );
            assert( strcmp( sid_cstr( again ), canonical ) == 0 );
            assert( sid_is_canonical( again, canonical, strlen( canonical ) ) );
        }

        // Exercise stats code paths (cannot assert exact values without internal access)
        sid_print_stats( stdout );
        sid_reset_stats();

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Verify hash collision handling
    /**************************************************************/
    {
        sid_init();

        // Find strings that hash to same bucket (if possible)
        // Or just test many strings to ensure collisions work
        sid_t sids[ 1000 ];
        char  key[ 32 ];
        for ( int i = 0; i < 1000; i++ )
        {
            snprintf( key, sizeof( key ), "test_%d", i );
            sids[ i ] = sid_intern_cstr( key );
            assert( !sid_equals( sids[ i ], SID_INVALID ));
        }

        // Verify all are unique and retrievable
        for ( int i = 0; i < 1000; i++ )
        {
            snprintf( key, sizeof( key ), "test_%d", i );
            sid_t lookup = sid_intern_cstr( key );
            assert( sid_equals( lookup, sids[ i ] ));
        }

        sid_print_stats( stdout );
        sid_reset_stats();

        sid_shutdown();
    }
    /**************************************************************/
    // Test: Print statistics
    /**************************************************************/
    {
        sid_init();
        sid_print_stats( stdout );
        sid_shutdown();
    }
    /**************************************************************/
    // Test: Test usage example
    /**************************************************************/
    {
        sid_init();
        hash_perf_test();
        sid_shutdown();
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