/*==============================================================================================

    runtime_service/asset/asset_registry.c -- asset registry: id table, path dedup, refcount,
    state, and extension -> type dispatch.  Synchronous load (Phase 2); LOADING/id indirection
    are reserved so a background loader slots in later without an API change.

    Storage lives in file-scope globals (a static service is never hot-reloaded, so preserved
    module state is unnecessary -- matches draw).  core() (fs + sid + alloc) is reached through
    the cached gateway pointer declared in asset.c.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*==============================================================================================
    Records + type table + path index
==============================================================================================*/

typedef struct asset_type_s
{
    char            name[ ASSET_TYPE_NAME ];
    char            exts[ ASSET_TYPE_EXTS ][ ASSET_EXT_LEN ];   // lowercased, incl. leading '.'
    u32             ext_count;
    asset_load_fn   load;
    asset_unload_fn unload;
    void*           userdata;
    bool            used;

} asset_type_t;

typedef struct asset_rec_s
{
    sid_t  path;         // interned normalized vpath (dedup key + reload lookup)
    u32    hash;         // sid hash of the path (path-index key)
    u16    type;         // index into s_types (0 = none)
    u8     state;        // asset_state_t
    u8     used;
    i32    refcount;
    void*  resource;     // typed backend handle from the type's load fn
    u32    bytes;
    u64    mtime;        // source mtime at load, for hot-reload compare
    u32    generation;   // bumped on slot recycle -> stale-handle detection

} asset_rec_t;

/* Open-addressing path index: sid-hash -> record slot.  state: 0 empty, 1 used, 2 tombstone. */
#define ASSET_INDEX_CAP    2048   // power of two, > ASSET_MAX for a healthy load factor
typedef struct asset_index_s
{
    u32 hash;
    u16 rec;
    u8  state;

} asset_index_t;

static asset_type_t   s_types[ ASSET_TYPE_MAX ];
static u32            s_type_count;        // includes reserved slot 0
static asset_rec_t    s_recs[ ASSET_MAX ];
static u32            s_live;              // live record count
static asset_index_t  s_index[ ASSET_INDEX_CAP ];

/*==============================================================================================
    Path helpers
==============================================================================================*/

/* Normalize a vpath into `out` for dedup: '\\' -> '/', strip leading slashes. */
static void
asset_norm( char* out, u32 out_cap, const char* in )
{
    while ( *in == '/' || *in == '\\' )
        ++in;

    u32 n = 0;
    for ( ; in[ 0 ] && n < out_cap - 1; ++in )
        out[ n++ ] = ( *in == '\\' ) ? '/' : *in;
    out[ n ] = '\0';
}

/* Extension of a path (".png"), lowercased into `out`; "" if none. */
static void
asset_ext_of( char* out, u32 out_cap, const char* path )
{
    const char* slash = strrchr( path, '/' );
    const char* base  = slash ? slash + 1 : path;
    const char* dot   = strrchr( base, '.' );
    out[ 0 ] = '\0';
    if ( !dot )
        return;

    u32 n = 0;
    for ( ; dot[ 0 ] && n < out_cap - 1; ++dot )
        out[ n++ ] = ( char )tolower( ( unsigned char )*dot );
    out[ n ] = '\0';
}

/* Find the type that claims `ext` (already lowercased), or 0 (none). */
static u16
asset_type_for_ext( const char* ext )
{
    if ( !ext[ 0 ] )
        return 0;
    for ( u32 t = 1; t < s_type_count; ++t )
    {
        if ( !s_types[ t ].used )
            continue;
        for ( u32 e = 0; e < s_types[ t ].ext_count; ++e )
        {
            if ( strcmp( s_types[ t ].exts[ e ], ext ) == 0 )
                return ( u16 )t;
        }
    }
    return 0;
}

/*==============================================================================================
    Path index
==============================================================================================*/

static i32
asset_index_find( sid_t path, u32 hash )
{
    u32 mask = ASSET_INDEX_CAP - 1;
    u32 i    = hash & mask;
    for ( u32 n = 0; n < ASSET_INDEX_CAP; ++n )
    {
        asset_index_t* s = &s_index[ i ];
        if ( s->state == 0 )
            return -1;    // empty: not present
        if ( s->state == 1 && s->hash == hash && sid_equals( s_recs[ s->rec ].path, path ) )
            return ( i32 )s->rec;
        i = ( i + 1 ) & mask;    // tombstone (2) or miss: keep probing
    }
    return -1;
}

static void
asset_index_insert( u32 hash, u16 rec )
{
    u32 mask = ASSET_INDEX_CAP - 1;
    u32 i    = hash & mask;
    for ( u32 n = 0; n < ASSET_INDEX_CAP; ++n )
    {
        asset_index_t* s = &s_index[ i ];
        if ( s->state != 1 )    // reuse empty or tombstone
        {
            s->state = 1;
            s->hash  = hash;
            s->rec   = rec;
            return;
        }
        i = ( i + 1 ) & mask;
    }
}

static void
asset_index_remove( sid_t path, u32 hash )
{
    u32 mask = ASSET_INDEX_CAP - 1;
    u32 i    = hash & mask;
    for ( u32 n = 0; n < ASSET_INDEX_CAP; ++n )
    {
        asset_index_t* s = &s_index[ i ];
        if ( s->state == 0 )
            return;
        if ( s->state == 1 && s->hash == hash && sid_equals( s_recs[ s->rec ].path, path ) )
        {
            s->state = 2;    // tombstone -- keeps probe chains intact
            return;
        }
        i = ( i + 1 ) & mask;
    }
}

/*==============================================================================================
    Record helpers
==============================================================================================*/

static asset_rec_t*
asset_rec_from_id( asset_id_t id )
{
    if ( id.index == 0 || id.index > ASSET_MAX )
        return NULL;
    asset_rec_t* r = &s_recs[ id.index - 1 ];
    if ( !r->used || r->generation != id.generation )
        return NULL;
    return r;
}

/* Load bytes and decode into the record's resource; sets state LOADED or FAILED. */
static void
asset_do_load( asset_rec_t* r )
{
    r->state = ASSET_LOADING;

    const char* vpath = core()->sid_cstr( r->path );
    fs_blob_t   blob  = fs()->read( vpath );
    if ( !blob.ok )
    {
        LOG_WARN( "asset: read failed for '%s'", vpath );
        r->resource = NULL;
        r->bytes    = 0;
        r->state    = ASSET_FAILED;
        return;
    }

    fs_stat_t st;
    if ( fs()->stat( vpath, &st ) )
        r->mtime = st.mtime;
    r->bytes = blob.size;

    asset_type_t* t   = &s_types[ r->type ];
    void*         res = ( t->used && t->load ) ? t->load( vpath, blob.data, blob.size, t->userdata ) : NULL;
    fs()->free( &blob );

    if ( res )
    {
        r->resource = res;
        r->state    = ASSET_LOADED;
    }
    else
    {
        r->resource = NULL;
        r->state    = ASSET_FAILED;
        LOG_WARN( "asset: no loader / decode failed for '%s' (type %u)", vpath, r->type );
    }
}

static void
asset_do_unload( asset_rec_t* r )
{
    asset_type_t* t = &s_types[ r->type ];
    if ( r->resource && t->used && t->unload )
        t->unload( r->resource, t->userdata );
    r->resource = NULL;
    r->state    = ASSET_UNLOADED;
}

/*==============================================================================================
    Lifecycle  (driven by asset_mod_init / asset_mod_exit)
==============================================================================================*/

static void
asset_system_init( void )
{
    memset( s_types, 0, sizeof( s_types ) );
    memset( s_recs, 0, sizeof( s_recs ) );
    memset( s_index, 0, sizeof( s_index ) );
    s_type_count = 1;    // slot 0 reserved as "none"
    s_live       = 0;
}

static void
asset_system_exit( void )
{
    for ( u32 i = 0; i < ASSET_MAX; ++i )
    {
        if ( s_recs[ i ].used )
            asset_do_unload( &s_recs[ i ] );
    }
    memset( s_recs, 0, sizeof( s_recs ) );
    memset( s_index, 0, sizeof( s_index ) );
    s_live = 0;
}

/*==============================================================================================
    Public API implementation
==============================================================================================*/

static u16
asset_type_register( const char* name, const char* const* exts, u32 ext_count,
                     asset_load_fn load, asset_unload_fn unload, void* userdata )
{
    if ( s_type_count >= ASSET_TYPE_MAX )
    {
        LOG_ERROR( "asset: type table full (%d), cannot register '%s'", ASSET_TYPE_MAX, name ? name : "?" );
        return 0;
    }

    u16           id = ( u16 )s_type_count++;
    asset_type_t* t  = &s_types[ id ];
    t->used     = true;
    t->load     = load;
    t->unload   = unload;
    t->userdata = userdata;
    snprintf( t->name, ASSET_TYPE_NAME, "%s", name ? name : "" );

    u32 count = ext_count;
    if ( count > ASSET_TYPE_EXTS )
    {
        /* Loud, not silent: a dropped extension dispatches to no type and FAILs at acquire,
           which is miserable to trace back here. */
        LOG_WARN( "asset: type '%s' registers %u extensions, cap is %d -- extras dropped",
                  name ? name : "?", ext_count, ASSET_TYPE_EXTS );
        count = ASSET_TYPE_EXTS;
    }
    for ( u32 e = 0; e < count; ++e )
    {
        /* store lowercased, ensuring a leading '.' */
        const char* src = exts[ e ] ? exts[ e ] : "";
        char*       dst = t->exts[ e ];
        u32         n   = 0;
        if ( src[ 0 ] && src[ 0 ] != '.' && n < ASSET_EXT_LEN - 1 )
            dst[ n++ ] = '.';
        for ( ; src[ 0 ] && n < ASSET_EXT_LEN - 1; ++src )
            dst[ n++ ] = ( char )tolower( ( unsigned char )*src );
        dst[ n ] = '\0';
    }
    t->ext_count = count;
    return id;
}

static asset_id_t
asset_acquire( const char* vpath_in )
{
    asset_id_t invalid = { 0, 0 };
    if ( !vpath_in || !vpath_in[ 0 ] )
        return invalid;

    char norm[ 256 ];
    asset_norm( norm, sizeof( norm ), vpath_in );

    sid_t path = core()->sid_intern( norm, ( int32_t )strlen( norm ) );
    u32   hash = core()->sid_get_hash( path );

    /* Dedup: an existing record for this path just takes another reference. */
    i32 found = asset_index_find( path, hash );
    if ( found >= 0 )
    {
        asset_rec_t* r = &s_recs[ found ];
        ++r->refcount;
        asset_id_t id = { ( u32 )found + 1, r->generation };
        return id;
    }

    /* Allocate a fresh slot. */
    i32 slot = -1;
    for ( u32 i = 0; i < ASSET_MAX; ++i )
    {
        if ( !s_recs[ i ].used )
        {
            slot = ( i32 )i;
            break;
        }
    }
    if ( slot < 0 )
    {
        LOG_ERROR( "asset: record table full (%d), cannot acquire '%s'", ASSET_MAX, norm );
        return invalid;
    }

    char ext[ ASSET_EXT_LEN ];
    asset_ext_of( ext, sizeof( ext ), norm );

    asset_rec_t* r = &s_recs[ slot ];
    r->used     = 1;
    r->path     = path;
    r->hash     = hash;
    r->type     = asset_type_for_ext( ext );
    r->refcount = 1;
    r->resource = NULL;
    r->bytes    = 0;
    r->mtime    = 0;
    r->state    = ASSET_UNLOADED;
    /* r->generation persists from the slot's previous life (0 on first use). */

    asset_index_insert( hash, ( u16 )slot );
    ++s_live;

    asset_do_load( r );

    asset_id_t id = { ( u32 )slot + 1, r->generation };
    return id;
}

static void
asset_release( asset_id_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    if ( !r )
        return;

    if ( --r->refcount > 0 )
        return;

    asset_do_unload( r );
    asset_index_remove( r->path, r->hash );

    ++r->generation;    // invalidate stale handles to this recycled slot
    r->used     = 0;
    r->refcount = 0;
    --s_live;
}

static void
asset_reload( asset_id_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    if ( !r )
        return;
    asset_do_unload( r );
    asset_do_load( r );
}

/* Hot-reload poll: re-stat every live record's source and re-run the loader in place for any
   whose file changed on disk (mtime differs) -- plus retry records that FAILED (e.g. the file
   was missing and has since appeared).  The id and refcount are preserved, so the swapped
   resource shows up behind get() with no handle churn.  Returns the number reloaded.

   Caller-driven cadence (no clock dep here): a host/editor calls this a few times a second, or
   a sandbox once a frame.  A momentarily-unreadable source (an editor mid-write) simply stats
   as gone this tick and is left untouched until it settles.  This is the mtime-compare fallback
   the plan names; an OS file watch could later gate which records get re-stat'd. */
static u32
asset_refresh( void )
{
    u32 reloaded = 0;
    for ( u32 i = 0; i < ASSET_MAX; ++i )
    {
        asset_rec_t* r = &s_recs[ i ];
        if ( !r->used )
            continue;

        const char* vpath = core()->sid_cstr( r->path );
        fs_stat_t   st;
        if ( !fs()->stat( vpath, &st ) )
            continue;    // source unavailable this tick -- keep the current resource

        if ( r->state != ASSET_FAILED && st.mtime == r->mtime )
            continue;    // unchanged since load

        LOG_INFO( "asset: hot-reload '%s' (mtime %llu -> %llu)", vpath,
                  ( unsigned long long )r->mtime, ( unsigned long long )st.mtime );
        asset_do_unload( r );
        asset_do_load( r );    // re-stats and updates r->mtime
        ++reloaded;
    }
    return reloaded;
}

static void*
asset_get( asset_id_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return ( r && r->state == ASSET_LOADED ) ? r->resource : NULL;
}

static int
asset_state( asset_id_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return r ? ( int )r->state : ASSET_UNLOADED;
}

static bool
asset_valid( asset_id_t id )
{
    return asset_rec_from_id( id ) != NULL;
}

static i32
asset_refcount( asset_id_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return r ? r->refcount : 0;
}

static u32
asset_count( void )
{
    return s_live;
}

/*============================================================================================*/
