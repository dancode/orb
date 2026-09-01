/*==============================================================================================

    engine/res/res_registry.c - Name pool, hash table, registration and lookup.

    Included by res.c after the storage block; not compiled on its own.

==============================================================================================*/
#ifndef RES_REGISTRY_C_PRELUDE
    #include "orb.h"
    #include <stdio.h>
    #include <string.h>
    #include "engine/res/res_host.h"
#endif

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
res_init( void )
{
    memset( &g_res, 0, sizeof( g_res ) );
    g_res_error[ 0 ] = 0;
}

void
res_exit( void )
{
    res_init();
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

    Linear probe from id & mask.  The table never fills past 50% (RES_HASH_SIZE >= 2x
    RES_MAX_ENTRIES), so a probe always terminates at an empty bucket.
==============================================================================================*/

/* Bucket index holding `id`, or the empty bucket where it would go. */
static u32
res_probe( rid_t id )
{
    u32 b = ( u32 )id & RES_HASH_MASK;
    for ( ;; )
    {
        u32 slot1 = g_res.hash[ b ];
        if ( slot1 == 0 || g_res.slots[ slot1 - 1 ].id == id )
            return b;
        b = ( b + 1 ) & RES_HASH_MASK;
    }
}

/* Slot for `id`, or NULL if unregistered. */
static const res_slot_t*
res_find( rid_t id )
{
    if ( id == RID_INVALID )
        return NULL;
    u32 slot1 = g_res.hash[ res_probe( id ) ];
    return slot1 ? &g_res.slots[ slot1 - 1 ] : NULL;
}

/*==============================================================================================
    Registration
==============================================================================================*/

rid_t
res_register_id( rid_t id, const char* name )
{
    char canon[ RES_NAME_MAX + 1 ];
    u32  len = res_canon( name, canon, sizeof( canon ) );

    if ( len == 0 )
    {
        res_set_error( "register: name is empty or longer than %d bytes", RES_NAME_MAX );
        return RID_INVALID;
    }
    if ( id == RID_INVALID )
    {
        res_set_error( "register '%s': id is RID_INVALID", canon );
        return RID_INVALID;
    }

    u32 b     = res_probe( id );
    u32 slot1 = g_res.hash[ b ];

    if ( slot1 )
    {
        /* Present. Same name: idempotent hit. Different name: collision -- the first
           registration stands, and both names are reported so the build can be fixed. */
        const res_slot_t* s = &g_res.slots[ slot1 - 1 ];
        if ( s->name_len == len && memcmp( g_res.pool + s->name_off, canon, len ) == 0 )
            return id;

        res_set_error( "rid collision 0x%08x: '%s' (registered) vs '%s' (rejected)",
                       ( unsigned )id, g_res.pool + s->name_off, canon );
        return RID_INVALID;
    }

    if ( g_res.count >= RES_MAX_ENTRIES )
    {
        res_set_error( "register '%s': catalogue full (%d entries) -- raise RES_MAX_ENTRIES",
                       canon, RES_MAX_ENTRIES );
        return RID_INVALID;
    }
    if ( g_res.pool_top + len + 1 > RES_NAME_POOL_SIZE )
    {
        res_set_error( "register '%s': name pool full (%d bytes) -- raise RES_NAME_POOL_SIZE",
                       canon, RES_NAME_POOL_SIZE );
        return RID_INVALID;
    }

    /* Copy the canonical text; the caller's buffer may not outlive this call. */
    res_slot_t* s = &g_res.slots[ g_res.count ];
    s->id         = id;
    s->name_off   = g_res.pool_top;
    s->name_len   = len;
    memcpy( g_res.pool + g_res.pool_top, canon, len + 1 );
    g_res.pool_top += len + 1;

    g_res.hash[ b ] = ++g_res.count; /* store index + 1 */
    return id;
}

rid_t
res_register( const char* name )
{
    if ( !name || !name[ 0 ] )
    {
        res_set_error( "register: name is empty" );
        return RID_INVALID;
    }
    return res_register_id( res_hash_name( name ), name );
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
    return g_res.count;
}

void
res_each( res_each_fn fn, void* user )
{
    if ( !fn )
        return;
    for ( u32 i = 0; i < g_res.count; ++i )
        fn( g_res.slots[ i ].id, g_res.pool + g_res.slots[ i ].name_off, user );
}

/*============================================================================================*/
