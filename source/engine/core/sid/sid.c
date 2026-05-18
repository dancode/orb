// clang-format off
/*==============================================================================================

    sid.c : configuration

    Note: sid_hash / sid_hash_len are static inline in sid.h - no plumbing required,
    so any TU (including DLLs and codegen) can compute hashes directly.

==============================================================================================*/

#define DEFAULT_TABLE_SIZE ( 512 )                  // initial entry hash table size (power of 2)
#define DEFAULT_MAX_LOAD   0.70f                    // rehash when 70% full or 50% for perf

#define DEFAULT_ARENA_SIZE ( 4096 )                 // initial size of string pool in bytes.
#define MAX_ARENA_SIZE     ( 16 * 1024 * 1024 )     // max 16MB arena
#define MAX_STRING_LENGTH  255                      // string length limit

/*==============================================================================================

    sid.c : declarations

==============================================================================================*/

static inline unsigned char
ascii_tolower( unsigned char c )
{
    return ( c >= 'A' && c <= 'Z' ) ? ( c + 32 ) : c;
}

intern_state_t g_intern;    // global intern state (must be public for natvis debug)

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
        assert_msg( false, "String arena exceeded maximum size" );
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

static bool sid_is_init = false;

void
sid_init( void )
{
    sid_is_init = true;

    /* Initialize global intern state */
    memset( &g_intern, 0, sizeof( g_intern ) );

    ORB_STATIC_ASSERT( ( DEFAULT_TABLE_SIZE & ( DEFAULT_TABLE_SIZE - 1 ) ) == 0, "DEFAULT_TABLE_SIZE must be a power of two" );

    /* initialize table and arena */
    if ( table_init( &g_intern.table, DEFAULT_TABLE_SIZE ) == false )
    {
        assert_msg( false, "Failed to initialize intern table" );
        return;
    }
    if ( arena_init( &g_intern.arena, DEFAULT_ARENA_SIZE ) == false )
    {
        free( g_intern.table );
        assert_msg( false, "Failed to initialize string arena" );
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
sid_exit( void )
{
    assert( sid_is_init == true );
    sid_is_init = false;

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
    assert( sid_is_init == true );

    /* intern string with given length */
    /* Note: we always return the CANONICAL version (the first interned) */

    if ( str == NULL )
    {
        assert_msg( false, "NULL string passed to sid_intern" );
        return SID_INVALID;
    }
    if ( len < 0 || len > MAX_STRING_LENGTH )
    {
        assert_msg( false, "Invalid string length for interning" );
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
        assert_msg( false, "failed to grow hash table" );
        return SID_INVALID;
    }

    /* add string to arena first */
    sid_t new_sid = arena_add_string( &g_intern.arena, str, ( uint8_t )len );
    if ( sid_equals( new_sid, SID_INVALID ))
    {
        assert_msg( false, "string arena allocation failed" );
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
    assert( sid_is_init == true );

    if ( str == NULL ) return SID_INVALID;

    /* convenience function for C strings */
    return sid_intern( str, ( int32_t )strlen( str ) );
}

sid_t 
sid_find_cstr( const char* str )
{
    assert( sid_is_init == true );

    /* find existing interned string, or return SID_INVALID */
    if ( str == NULL )
        return SID_INVALID;

    uint32_t hash = sid_hash( str );
    uint32_t mask = g_intern.table_size - 1;
    uint32_t slot = hash & mask;
    uint32_t probe_length = 0;
    while ( true )
    {
        g_intern.total_lookups++;
        
        // update total probes
        intern_slot_t* entry = &g_intern.table[ slot ];
        if ( sid_equals( entry->sid, SID_INVALID ))
        {
            // empty slot, string not found
            return SID_INVALID;
        }

        // check for match (case-insensitive)
        const char* entry_str = sid_cstr( entry->sid );
        if ( str_equal_insensitive( entry_str, str, sid_length( entry->sid ) ))
        {            
            return entry->sid; // found it
        }
        
        // continue probing
        probe_length++;
        slot = ( slot + 1 ) & mask;
    }
    
    return SID_INVALID; // not found
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
            assert( !sid_equals( test_sids[ i ], SID_INVALID ) );
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
                assert( sid_equals( s, test_sids[ i ] ) );
            }

            // Random-ish access (worst case - jumps around memory)
            for ( int i = 300; i < 600; i++ )
            {
                int idx = 300 + ( ( i * 37 ) % 300 );    // pseudo-random
                snprintf( key, sizeof( key ), "transform_component_%d", idx );
                sid_t s = sid_intern_cstr( key );
                assert( !sid_equals( s, SID_INVALID ) );
            }

            // Repeated hot strings (cache-friendly)
            for ( int i = 0; i < 100; i++ )
            {
                snprintf( key, sizeof( key ), "player_%d", 600 + ( i % 10 ) );
                sid_t s = sid_intern_cstr( key );
                assert( !sid_equals( s, SID_INVALID ) );
            }

            // Different case variations (tests case-insensitive lookup)
            for ( int i = 0; i < 100; i++ )
            {
                snprintf( key, sizeof( key ), "KEY_%04d", i );    // uppercase
                sid_t s = sid_intern_cstr( key );
                assert( sid_equals( s, test_sids[ i ] ) );    // should match lowercase version
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
            if ( sid_equals( s, SID_INVALID ) )
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
// clang-format on