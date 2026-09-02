/*==============================================================================================

    runtime_service/asset/asset_registry.c -- asset registry: id table, name dedup, refcount,
    state, and type dispatch.  Synchronous load; LOADING/id indirection are reserved so a
    background loader slots in later without an API change.

    A record is one loaded instance of a resource, keyed by its name (interned through core's
    sid so equality is an integer compare), so every acquirer of one name shares it.  The
    record does not store a path.  Its vpath is the name plus the extension of its type that
    matched, recomposed whenever fs is asked (load, stat), which is what keeps this service
    ignorant of where content lives -- the mounts know.

    Storage lives in file-scope globals (a static service is never hot-reloaded, so preserved
    module state is unnecessary -- matches draw).  fs() and core() (logging, sid) are reached
    through the cached gateway pointers declared in asset.c.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*==============================================================================================
    Records + type table + name index
==============================================================================================*/

typedef struct asset_type_s
{
    char            name[ ASSET_TYPE_NAME ];
    char            exts[ ASSET_TYPE_EXTS ][ ASSET_EXT_LEN ];   // lowercased, incl. leading '.', preference order
    u32             ext_count;
    asset_load_fn   load;
    asset_unload_fn unload;
    void*           userdata;
    bool            used;

} asset_type_t;

typedef struct asset_rec_s
{
    sid_t  name;         // the resource this is an instance of: its name, interned (dedup key)
    u16    type;         // index into s_types, named by the caller at acquire
    u8     ext;          // index into the type's exts of the file that loaded (valid when LOADED)
    u8     state;        // asset_state_t
    u8     used;
    i32    refcount;
    void*  resource;     // typed backend handle from the type's load fn
    u32    bytes;
    u64    mtime;        // source mtime at load, for hot-reload compare
    u32    generation;   // bumped on slot recycle -> stale-handle detection

} asset_rec_t;

/* Open-addressing name index: sid offset -> record slot.  state: 0 empty, 1 used, 2 tombstone. */
#define ASSET_INDEX_CAP    2048   // power of two, > ASSET_MAX for a healthy load factor
typedef struct asset_index_s
{
    u32   key;           // sid_t.off of the record's name
    u16   rec;
    u8    state;

} asset_index_t;

static asset_type_t   s_types[ ASSET_TYPE_MAX ];
static u32            s_type_count;        // includes reserved slot 0
static asset_rec_t    s_recs[ ASSET_MAX ];
static u32            s_live;              // live record count
static asset_index_t  s_index[ ASSET_INDEX_CAP ];

/*==============================================================================================
    Name index

    Keyed by the interned name's sid offset: two acquires of one name intern to one sid, so
    the key is exact (no hash-collision case to handle).  The offset is mixed before masking
    because sid offsets are pool positions, not hashes.
==============================================================================================*/

static u32
asset_index_slot( u32 key )
{
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    return key & ( ASSET_INDEX_CAP - 1 );
}

static i32
asset_index_find( u32 key )
{
    u32 mask = ASSET_INDEX_CAP - 1;
    u32 i    = asset_index_slot( key );
    for ( u32 n = 0; n < ASSET_INDEX_CAP; ++n )
    {
        asset_index_t* s = &s_index[ i ];
        if ( s->state == 0 )
            return -1;    // empty: not present
        if ( s->state == 1 && s->key == key )
            return ( i32 )s->rec;
        i = ( i + 1 ) & mask;    // tombstone (2) or miss: keep probing
    }
    return -1;
}

static void
asset_index_insert( u32 key, u16 rec )
{
    u32 mask = ASSET_INDEX_CAP - 1;
    u32 i    = asset_index_slot( key );
    for ( u32 n = 0; n < ASSET_INDEX_CAP; ++n )
    {
        asset_index_t* s = &s_index[ i ];
        if ( s->state != 1 )    // reuse empty or tombstone
        {
            s->state = 1;
            s->key   = key;
            s->rec   = rec;
            return;
        }
        i = ( i + 1 ) & mask;
    }
}

static void
asset_index_remove( u32 key )
{
    u32 mask = ASSET_INDEX_CAP - 1;
    u32 i    = asset_index_slot( key );
    for ( u32 n = 0; n < ASSET_INDEX_CAP; ++n )
    {
        asset_index_t* s = &s_index[ i ];
        if ( s->state == 0 )
            return;
        if ( s->state == 1 && s->key == key )
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
asset_rec_from_id( aid_t id )
{
    if ( id.index == 0 || id.index > ASSET_MAX )
        return NULL;
    asset_rec_t* r = &s_recs[ id.index - 1 ];
    if ( !r->used || r->generation != id.generation )
        return NULL;
    return r;
}

/* The record's name, from core's interner. */
static const char*
asset_rec_name( const asset_rec_t* r )
{
    return core()->sid_cstr( r->name );
}

/* The vpath of `r` through extension `ext` of its type: name + ".ext". */
static void
asset_path( const asset_rec_t* r, u32 ext, char* out, u32 cap )
{
    res_path( out, cap, asset_rec_name( r ), s_types[ r->type ].exts[ ext ] );
}

/* The type's extensions as one string for a diagnostic: ".tex .png .jpg". */
static void
asset_exts_join( const asset_type_t* t, char* out, u32 cap )
{
    u32 n = 0;
    out[ 0 ] = '\0';
    for ( u32 e = 0; e < t->ext_count && n + 1 < cap; ++e )
        n += ( u32 )snprintf( out + n, cap - n, "%s%s", e ? " " : "", t->exts[ e ] );
}

/* Probe the type's extensions in preference order, read the first file fs has, and decode it
   into the record's resource; sets state LOADED or FAILED and records which extension won. */
static void
asset_do_load( asset_rec_t* r )
{
    r->state    = ASSET_LOADING;
    r->resource = NULL;
    r->bytes    = 0;

    asset_type_t* t = &s_types[ r->type ];
    char          path[ ASSET_PATH_MAX ];
    fs_blob_t     blob = { 0 };
    u32           e    = 0;
    for ( ; e < t->ext_count; ++e )
    {
        asset_path( r, e, path, sizeof( path ) );
        if ( !fs()->exists( path ) )
            continue;
        blob = fs()->read( path );
        if ( blob.ok )
            break;
    }

    if ( !blob.ok )
    {
        char exts[ ASSET_TYPE_EXTS * ASSET_EXT_LEN ];
        asset_exts_join( t, exts, sizeof( exts ) );
        LOG_WARN( "asset: no file for '%s' as %s (tried %s)", asset_rec_name( r ), t->name, exts );
        r->state = ASSET_FAILED;
        return;
    }
    r->ext = ( u8 )e;

    fs_stat_t st;
    if ( fs()->stat( path, &st ) )
        r->mtime = st.mtime;
    r->bytes = blob.size;

    void* res_ptr = ( t->used && t->load ) ? t->load( path, blob.data, blob.size, t->userdata ) : NULL;
    fs()->free( &blob );

    if ( res_ptr )
    {
        r->resource = res_ptr;
        r->state    = ASSET_LOADED;
        LOG_INFO( "asset: loaded '%s' as %s", path, t->name );
    }
    else
    {
        r->resource = NULL;
        r->state    = ASSET_FAILED;
        LOG_WARN( "asset: decode failed for '%s' as %s", path, t->name );
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
        /* Loud, not silent: a dropped extension is a file the type can never find, which is
           miserable to trace back here. */
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

static aid_t
asset_acquire( const char* name, u16 type )
{
    aid_t invalid = { 0, 0 };

    /* Names are canonical (lowercase, '/' separators, no extension) and nothing folds: a
       misspelled name is refused here rather than probed against fs under a spelling no
       content file can have. */
    if ( !res_name_ok( name ) )
    {
        LOG_ERROR( "asset: acquire '%s': not a canonical resource name (lowercase, '/' separators)",
                   name ? name : "(null)" );
        return invalid;
    }

    if ( type == 0 || type >= s_type_count || !s_types[ type ].used )
    {
        LOG_ERROR( "asset: acquire '%s': unknown asset type %u", name, type );
        return invalid;
    }

    /* Dedup: an existing record for this name just takes another reference. */
    sid_t sid   = core()->sid_intern_cstr( name );
    i32   found = asset_index_find( sid.off );
    if ( found >= 0 )
    {
        asset_rec_t* r = &s_recs[ found ];
        if ( r->type != type )
        {
            LOG_ERROR( "asset: '%s' is already loaded as %s, not %s -- one resource, one type", name,
                       s_types[ r->type ].name, s_types[ type ].name );
            return invalid;
        }
        ++r->refcount;
        aid_t id = { ( u32 )found + 1, r->generation };
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
        LOG_ERROR( "asset: record table full (%d), cannot acquire '%s'", ASSET_MAX, name );
        return invalid;
    }

    asset_rec_t* r = &s_recs[ slot ];
    r->used     = 1;
    r->name     = sid;
    r->type     = type;
    r->ext      = 0;
    r->refcount = 1;
    r->resource = NULL;
    r->bytes    = 0;
    r->mtime    = 0;
    r->state    = ASSET_UNLOADED;
    /* r->generation persists from the slot's previous life (0 on first use). */

    asset_index_insert( sid.off, ( u16 )slot );
    ++s_live;

    asset_do_load( r );

    aid_t id = { ( u32 )slot + 1, r->generation };
    return id;
}

static void
asset_release( aid_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    if ( !r )
        return;

    if ( --r->refcount > 0 )
        return;

    asset_do_unload( r );
    asset_index_remove( r->name.off );

    ++r->generation;    // invalidate stale handles to this recycled slot
    r->used     = 0;
    r->refcount = 0;
    --s_live;
}

static void
asset_reload( aid_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    if ( !r )
        return;
    asset_do_unload( r );
    asset_do_load( r );
}

/* Hot-reload poll: re-stat every live record's file and re-run the loader in place for any
   whose file changed on disk (mtime differs) -- plus retry records that FAILED (e.g. the file
   was missing and has since appeared, under any of the type's extensions).  The id and
   refcount are preserved, so the swapped resource shows up behind get() with no handle churn.
   Returns the number reloaded.

   Caller-driven cadence (no clock dep here): a host/editor calls this a few times a second, or
   a sandbox once a frame.  A momentarily-unreadable source (an editor mid-write) simply stats
   as gone this tick and is left untouched until it settles.  Only the extension that loaded is
   stat'd: a preferred form appearing beside it is picked up on the next reload, not here. */
static u32
asset_refresh( void )
{
    u32 reloaded = 0;
    for ( u32 i = 0; i < ASSET_MAX; ++i )
    {
        asset_rec_t* r = &s_recs[ i ];
        if ( !r->used )
            continue;

        if ( r->state != ASSET_FAILED )
        {
            char      path[ ASSET_PATH_MAX ];
            fs_stat_t st;
            asset_path( r, r->ext, path, sizeof( path ) );
            if ( !fs()->stat( path, &st ) )
                continue;    // source unavailable this tick -- keep the current resource
            if ( st.mtime == r->mtime )
                continue;    // unchanged since load

            LOG_INFO( "asset: hot-reload '%s' (mtime %llu -> %llu)", path,
                      ( unsigned long long )r->mtime, ( unsigned long long )st.mtime );
        }

        asset_do_unload( r );
        asset_do_load( r );    // re-probes the extensions, re-stats, updates r->mtime
        if ( r->state == ASSET_LOADED )
            ++reloaded;
    }
    return reloaded;
}

static void*
asset_get( aid_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return ( r && r->state == ASSET_LOADED ) ? r->resource : NULL;
}

static int
asset_state( aid_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return r ? ( int )r->state : ASSET_UNLOADED;
}

static bool
asset_valid( aid_t id )
{
    return asset_rec_from_id( id ) != NULL;
}

static i32
asset_refcount( aid_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return r ? r->refcount : 0;
}

static const char*
asset_name( aid_t id )
{
    asset_rec_t* r = asset_rec_from_id( id );
    return r ? asset_rec_name( r ) : NULL;
}

static u32
asset_count( void )
{
    return s_live;
}

/*============================================================================================*/
