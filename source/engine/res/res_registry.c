/*==============================================================================================

    engine/res/res_registry.c - Name pool, hash table, registration and lookup.

    Included by res.c after the storage block; not compiled on its own.

==============================================================================================*/
#ifndef RES_REGISTRY_C_PRELUDE
    #include "orb.h"
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include "engine/res/res_host.h"
#endif

// clang-format off
/*==============================================================================================
    Errors
==============================================================================================*/

static void
res_set_error( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( g_res_error, sizeof( g_res_error ), fmt, args );
    va_end( args );
    fprintf( stderr, "res: %s\n", g_res_error );
}

const char*
res_last_error( void )
{
    return g_res_error;
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

void
res_exit( void )
{
    free( g_res.hash );
    free( g_res.slots );
    free( g_res.pool );
    memset( &g_res, 0, sizeof( g_res ) );
    g_res_error[ 0 ] = 0;
}

void
res_init( void )
{
    /* Both mean "empty catalogue"; storage comes back on the next registration. */
    res_exit();
}

/*==============================================================================================
    Canonical form
==============================================================================================*/

u32
res_canon( const char* name, char* out, u32 cap )
{
    if ( !name || !out || cap == 0 )
        return 0;

    u32 len = 0;
    while ( name[ len ] )
    {
        if ( len + 1 >= cap )
        {
            out[ 0 ] = 0;
            return 0;
        }
        out[ len ] = res_canon_char( name[ len ] );
        len++;
    }
    out[ len ] = 0;
    return len;
}

/*==============================================================================================
    Hash table

    Linear probe from id & mask.  Growth keeps hash_size at twice slot_cap, so the table never
    fills past 50% and a probe always terminates at an empty bucket.
==============================================================================================*/

/* Bucket index holding `id`, or the empty bucket where it would go. Requires a live table. */

static u32
res_probe( rid_t id )
{
    u32 mask = g_res.hash_size - 1;
    u32 b    = ( u32 )id & mask;
    for ( ;; )
    {
        u16  slot1 = g_res.hash[ b ];
        if ( slot1 == 0 || g_res.slots[ slot1 - 1 ].id == id )
             return b; /* empty or matched -- a valid slot */
        b = ( b + 1 ) & mask;
    }
}

/* Slot for `id`, or NULL if unregistered. */
static const res_slot_t*
res_find( rid_t id )
{
    if ( id == RID_INVALID || g_res.slot_count == 0 )
         return NULL;

    u16 slot1 = g_res.hash[ res_probe( id ) ];
    return slot1 ? &g_res.slots[ slot1 - 1 ] : NULL;
}

/*==============================================================================================
    Growth

    Doubling, on the sid model.  Both reserves run to completion before an insert touches the
    table, because rehashing moves every bucket and would strand a probe taken beforehand.
==============================================================================================*/

/* Make room for one more slot, rebuilding the hash table at the new size. */

static bool
res_reserve_slot( void )
{
    if ( g_res.slot_count < g_res.slot_cap )
         return true;

    /* Both caps are powers of two, so doubling keeps slot_cap one as well -- the probe mask
       below depends on that. The ceiling is reached, never overshot. */

    u32  slot_cap = g_res.slot_cap ? g_res.slot_cap * 2 : RES_INIT_ENTRIES;
    if ( slot_cap > RES_MAX_ENTRIES )
    {
         res_set_error( "register: catalogue full (%d entries) -- the u16 bucket index is spent", 
                        RES_MAX_ENTRIES );
         return false;
    }

    /* Both blocks land before any of the registry is repointed: committing a wider slot_cap
       against the old, narrower table would let the load factor pass 50% and hang a probe. */

    u32  hash_size = slot_cap * 2;
    u16* hash = ( u16* )calloc( hash_size, sizeof( u16 ) );
    if ( hash == NULL ) 
    {
        res_set_error( "register: out of memory growing to %u buckets", hash_size );
        return false;
    }

    res_slot_t* slots = ( res_slot_t* )realloc( g_res.slots, slot_cap * sizeof( res_slot_t ) );
    if ( slots == NULL )
    {
        free( hash );
        res_set_error( "register: out of memory growing to %u slots", slot_cap );
        return false;
    }

    free( g_res.hash );
    g_res.hash      = hash;
    g_res.hash_size = hash_size;
    g_res.slots     = slots;
    g_res.slot_cap  = slot_cap;

    /* Reinsert every live slot. Ids are unique, so each probe lands on an empty bucket. */

    for ( u32 i = 0; i < g_res.slot_count; ++i )
    {
        u32 b = ( u32 )g_res.slots[ i ].id & ( hash_size - 1 );
        while ( g_res.hash[ b ] ) {
            b = ( b + 1 ) & ( hash_size - 1 );
        }
        g_res.hash[ b ] = ( u16 )( i + 1 );
    }
    return true;
}

/* Make room for `need` more bytes of name text. */

static bool
res_reserve_pool( u32 need )
{
    if ( g_res.pool && g_res.pool_top + need <= g_res.pool_cap )
         return true;

    u32 pool_cap = g_res.pool_cap ? g_res.pool_cap * 2 : RES_INIT_POOL;
    while ( g_res.pool_top + need > pool_cap )
    {
        pool_cap *= 2;
    }
    char* pool = ( char* )realloc( g_res.pool, pool_cap );
    if (  pool == NULL )
    {
        res_set_error( "register: out of memory growing the name pool to %u bytes", pool_cap );
        return false;
    }
    g_res.pool     = pool;
    g_res.pool_cap = pool_cap;
    return true;
}

/* Copy len + 1 bytes of `s` (its NUL included) to the top of the pool; returns the offset.
   The caller has reserved the room. */

static u32
res_pool_push( const char* s, u32 len )
{
    u32 off = g_res.pool_top;
    memcpy( g_res.pool + off, s, len + 1 );
    g_res.pool_top += len + 1;
    return off;
}

/*==============================================================================================
    Registration
==============================================================================================*/

/* Canonical form of `name` into out[RES_NAME_MAX + 1]. Returns the length, or 0 having
   reported why. Both entry points fold here so they refuse the same inputs. */

static u32
res_canon_checked( const char* name, char* out )
{
    u32  len = res_canon( name, out, RES_NAME_MAX + 1 );
    if ( len == 0 )
        res_set_error( "register: name is empty or longer than %d bytes", RES_NAME_MAX );
    return len;
}

/* Insert an already-canonical name under `id`. Returns id, or RID_INVALID having reported. */

static rid_t
res_insert( rid_t id, const char* canon, u32 len )
{
    if ( id == RID_INVALID )
    {
        res_set_error( "register '%s': id is RID_INVALID", canon );
        return RID_INVALID;
    }

    /* Present. Same name: idempotent hit -- this is every name of a hot-reloaded module's
       table on its second and later loads. Different name: collision, so the first
       registration stands and both names are reported for the build to fix. */

    const res_slot_t* hit = res_find( id );
    if ( hit )
    {
        if ( strcmp( g_res.pool + hit->name_off, canon ) != 0 )
        {
            res_set_error( "rid collision 0x%08x: '%s' (registered) vs '%s' (rejected)",
                            id, g_res.pool + hit->name_off, canon );
            return RID_INVALID;
        }
        return id;
    }

    /* Room for the slot and the name before anything is committed, so a failed growth
       leaves the catalogue exactly as it was. */
    if ( !res_reserve_slot() || !res_reserve_pool( len + 1 ) )
        return RID_INVALID;

    res_slot_t* s = &g_res.slots[ g_res.slot_count ];
    s->id         = id;
    s->name_off   = res_pool_push( canon, len );

    /* Bucket holds the slot index + 1. */
    g_res.hash[ res_probe( id ) ] = ( u16 )( ++g_res.slot_count );
    return id;
}

rid_t
res_register_id( rid_t id, const char* name )
{
    char canon[ RES_NAME_MAX + 1 ];

    u32  len = res_canon_checked( name, canon );
    if ( len == 0 )
         return RID_INVALID;

    /* The id is a pure function of the name, so a caller-supplied one that disagrees was
       computed by some other hash -- a stale tool, a foreign cooker -- and registering it
       would file the name under an id no RID() site can produce. */
    rid_t expect = res_hash_name( canon );
    if ( id != expect )
    {
        res_set_error( "register '%s': id 0x%08x does not hash from the name (expected 0x%08x)",
                       canon, id, expect );
        return RID_INVALID;
    }

    return res_insert( id, canon, len );
}

rid_t
res_register( const char* name )
{
    char canon[ RES_NAME_MAX + 1 ];

    u32  len = res_canon_checked( name, canon );
    if ( len == 0 )
        return RID_INVALID;

    /* Hash the folded text rather than the raw name: identical result, since
       res_hash_name folds as it goes, but it reads a short buffer already in
       cache instead of walking the caller's string a second time. */

    return res_insert( res_hash_name( canon ), canon, len );
}

u32
res_register_table( const res_table_t* table )
{
    if ( !table || !table->entries )
        return 0;

    u32 ok = 0;
    for ( u32 i = 0; i < table->count; ++i )
    {
        if ( res_register( table->entries[ i ].name ) != RID_INVALID )
            ok++;
    }
    return ok;
}

/*==============================================================================================
    Lookup
==============================================================================================*/

const char*
res_name( rid_t id )
{
    const res_slot_t* s = res_find( id );
    return s ? g_res.pool + s->name_off : NULL;
}

bool
res_exists( rid_t id )
{
    return res_find( id ) != NULL;
}

u32
res_count( void )
{
    return g_res.slot_count;
}

void
res_each( res_each_fn fn, void* user )
{
    if ( !fn ) return;
    for ( u32 i = 0; i < g_res.slot_count; ++i ) 
    {
        fn( g_res.slots[ i ].id, g_res.pool + g_res.slots[ i ].name_off, user );
    }
}

/*============================================================================================*/
// clang-format on