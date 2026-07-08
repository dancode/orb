/*==============================================================================================

    cvar_hash.c

    Cvar storage + lookup index -- the master cvar_t array and the open-addressing hash
    table that maps a name to its slot.

    - g_cvar_pool / g_cvar_count are the single source of truth for "every registered cvar";
      referenced directly (unity-shared statics) by registration, lookup, and the bulk
      value-modification loops in cvar.c.
    - Hash is initialized to HASH_EMPTY.
    - uses u16 indexes into the cvar instance pool.
    - HASH_TOMBSTONE is used for deletion.
    - insert: linear probe to next empty slot (or first tombstone).
    - find: linear probe until empty slot or match.

==============================================================================================*/
// clang-format off

/* Case-insensitive string compare helper */

static bool
cvar_str_icmp_eq( const char* a, const char* b )
{
    while ( *a && *b )
    {
        char ca = *a;
        if ( ca >= 'A' && ca <= 'Z' )
            ca = ca + ( 'a' - 'A' );

        char cb = *b;
        if ( cb >= 'A' && cb <= 'Z' )
            cb = cb + ( 'a' - 'A' );

        if ( ca != cb )
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Case-insensitive: does `name` start with `prefix`? Stops at the end of `prefix`, so `name`
   may be longer (e.g. tab-completion matching a registry name against a typed prefix). */

static bool
cvar_str_icmp_prefix( const char* name, const char* prefix )
{
    while ( *prefix )
    {
        char cn = *name, cp = *prefix;
        if ( cn >= 'A' && cn <= 'Z' ) cn = cn + ( 'a' - 'A' );
        if ( cp >= 'A' && cp <= 'Z' ) cp = cp + ( 'a' - 'A' );
        if ( cn != cp )
            return false;
        ++name;
        ++prefix;
    }
    return true;
}

/* Case-insensitive: does `needle` occur anywhere in `haystack`? Built on cvar_str_icmp_prefix
   tried at every starting offset (haystack/needle are short console strings, so this is cheap). */

static bool
cvar_str_icmp_find( const char* haystack, const char* needle )
{
    if ( !*needle )
        return true;

    for ( const char* h = haystack; *h; ++h )
    {
        if ( cvar_str_icmp_prefix( h, needle ) )
            return true;
    }
    return false;
}

static u32
cvar_hash( const char* s )
{
    u32 h = 2166136261u;    // FNV offset basis
    while ( *s )
    {
        char c = *s++;
        if ( c >= 'A' && c <= 'Z' )
            c = c + ( 'a' - 'A' );

        h ^= ( unsigned char )c;
        h *= 16777619u;    // FNV prime
    }
    return h;
}

/*==============================================================================================

    Cvar Hash Table - Open Addressing with Linear Probing

==============================================================================================*/

#define MAX_CVARS      256                      // Maximum registered cvars
#define HASH_SIZE      512                      // Hash table size (power of 2)
#define HASH_MASK      ( HASH_SIZE - 1 )        // Bit mask for hash

#define HASH_EMPTY     ( ( u16 )0xFFFF )        // Empty slot sentinel
#define HASH_TOMBSTONE ( ( u16 )0xFFFE )        // Deleted slot sentinel

static u32    g_cvar_count = 0;                 // Number of registered cvars
static cvar_t g_cvar_pool[ MAX_CVARS ];         // Fixed cvar array
static u16    global_cvar_hash[ HASH_SIZE ];    // Hash table of cvar indices

string_pool_t g_cvar_string_pool;               // String pool for cvar names and descriptions

/*============================================================================================*/
/* Initialize hash table */

static void
cvar_hash_init()
{
    for ( u32 i = 0; i < HASH_SIZE; ++i ) global_cvar_hash[ i ] = HASH_EMPTY;
}

/*============================================================================================*/
/* Find cvar by name using linear probing (returns NULL if not found) */

static cvar_t*
cvar_hash_find( const char* name )
{
    if ( !name )
        return NULL;

    u32  hash  = cvar_hash( name ) & HASH_MASK;
    const u32 start = hash;

    while ( true )
    {
        const u16 idx = global_cvar_hash[ hash ];
        if ( idx == HASH_EMPTY )
        {
            return NULL;    // Not found
        }

        if ( idx != HASH_TOMBSTONE )
        {
            cvar_t*     cv      = &g_cvar_pool[ idx ];
            const char* cv_name = string_pool_get( &g_cvar_string_pool, cv->name );
            if ( cvar_str_icmp_eq( cv_name, name ) )
            {
                return cv;    // Found cvar!
            }
        }

        hash = ( hash + 1 ) & HASH_MASK;
        if ( hash == start )
        {
            return NULL;    // Full loop
        }
    }
}

/*============================================================================================*/
/* Insert cvar into hash table by its cvar array id */

static void
cvar_hash_insert( u32 cvar_index )
{
    cvar_t*     cv         = &g_cvar_pool[ cvar_index ];
    const char* name       = string_pool_get( &g_cvar_string_pool, cv->name );

    u32         hash       = cvar_hash( name ) & HASH_MASK;
    u32         start      = hash;
    u32         first_tomb = ( u32 )-1;    // first free found if adding after not found.

    while ( true )
    {
        u16 slot = global_cvar_hash[ hash ];

        if ( slot == HASH_EMPTY )
        {
            if ( first_tomb != ( u32 )-1 )
            {
                // new entry goes into first tombstone found.
                global_cvar_hash[ first_tomb ] = ( u16 )cvar_index;
            }
            else
            {
                // current slot is new entry.
                global_cvar_hash[ hash ] = ( u16 )cvar_index;
            }
            return;
        }
        else if ( slot == HASH_TOMBSTONE )
        {
            if ( first_tomb == ( u32 )-1 )
                first_tomb = hash;
        }
        else
        {
            /* If duplicate insertion we do nothing */

            cvar_t* other = &g_cvar_pool[ slot ];
            if ( cvar_str_icmp_eq( string_pool_get( &g_cvar_string_pool, other->name ), name ) )
            {
                /* Duplicate found - shouldn't happen */
                return;
            }
        }

        hash = ( hash + 1 ) & HASH_MASK;
        if ( hash == start )
        {
            log_write( LOG_LEVEL_FATAL, "cvar", "hash table full while inserting cvar" );
            ORB_UNREACHABLE();
        }
    }
}

// clang-format on
