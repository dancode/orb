/*==============================================================================================

    sandbox/runtime/sb_asset_test/sb_asset_test.c -- asset service registry proof (Phase 2).

    Boots sys + ref + core + asset through the module system, mounts a scratch directory on
    core/fs, registers a trivial "blob" asset type, then exercises the registry:
      - acquire the same path twice -> same id, refcount 2  (dedup)
      - get() returns the decoded resource; state == LOADED
      - a different-cased / backslashed path folds to the SAME record
      - release twice -> record unloaded, count back to 0, stale handle rejected
      - acquiring a missing file yields a FAILED (but releasable) handle

    No renderer here; the "blob" loader just copies bytes into a small heap struct.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "engine/fs/fs_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/asset/asset_host.h"

/*==============================================================================================
    Trivial "blob" asset type -- proves the type_register / load / unload seam with no GPU.
==============================================================================================*/

typedef struct blob_res_s
{
    u32  size;
    char bytes[ 1 ];    // flexible-ish tail; allocated size = sizeof(u32) + size + 1

} blob_res_t;

static int g_blob_loads   = 0;    // instrumentation: how many times the loader ran
static int g_blob_unloads = 0;

static void*
blob_load( const char* vpath, const void* data, u32 size, void* userdata )
{
    UNUSED( vpath );
    UNUSED( userdata );

    blob_res_t* b = ( blob_res_t* )malloc( sizeof( u32 ) + size + 1 );
    if ( !b )
        return NULL;
    b->size = size;
    memcpy( b->bytes, data, size );
    b->bytes[ size ] = '\0';
    ++g_blob_loads;
    return b;
}

static void
blob_unload( void* resource, void* userdata )
{
    UNUSED( userdata );
    free( resource );
    ++g_blob_unloads;
}

/*==============================================================================================
    Proof
==============================================================================================*/

static const char*
state_name( int s )
{
    switch ( s )
    {
        case ASSET_UNLOADED: return "UNLOADED";
        case ASSET_LOADING:  return "LOADING";
        case ASSET_LOADED:   return "LOADED";
        case ASSET_FAILED:   return "FAILED";
        default:             return "?";
    }
}

static void
asset_test( void )
{
    printf( "\n=== asset service (registry) ===\n" );

    /* Scratch file in the CWD, served through a "data/" mount. */
    const char* probe_real = "asset_probe.tmp";
    const char* payload    = "orb asset phase 2 blob payload";
    u32         plen       = ( u32 )strlen( payload );
    if ( !sys_file_write_entire( probe_real, payload, plen ) )
    {
        printf( "  FAIL: could not write probe file\n" );
        return;
    }
    fs()->mount( "data/", "", 0 );

    /* Register the blob type for the ".blob" and ".tmp" extensions. */
    const char* exts[] = { ".blob", ".tmp" };
    u16         type   = asset()->type_register( "blob", exts, 2, blob_load, blob_unload, NULL );
    printf( "  registered type 'blob' -> id %u\n", type );

    /* 1) acquire + dedup: same path twice shares one record, refcount climbs to 2. */
    asset_id_t a = asset()->acquire( "data/asset_probe.tmp" );
    asset_id_t b = asset()->acquire( "data/asset_probe.tmp" );
    printf( "  acquire x2: a={%u,%u} b={%u,%u} same=%d  refcount=%d  count=%u  loads=%d\n",
            a.index, a.generation, b.index, b.generation,
            ( a.index == b.index && a.generation == b.generation ),
            asset()->refcount( a ), asset()->count(), g_blob_loads );

    /* 2) state + get: resource decoded once, readable, byte-identical. */
    blob_res_t* res   = ( blob_res_t* )asset()->get( a );
    bool        match = res && res->size == plen && memcmp( res->bytes, payload, plen ) == 0;
    printf( "  state=%s get.ok=%d size=%u match=%d\n",
            state_name( asset()->state( a ) ), res != NULL, res ? res->size : 0, match );

    /* 3) path normalization dedups: backslashes + mixed case -> same record, no reload. */
    asset_id_t c = asset()->acquire( "Data\\ASSET_PROBE.TMP" );
    printf( "  alt-form acquire: c={%u,%u} same-as-a=%d refcount=%d loads=%d (no reload)\n",
            c.index, c.generation, ( c.index == a.index ), asset()->refcount( a ), g_blob_loads );

    /* 4) release down to zero: three acquires (a,b,c) need three releases. */
    asset()->release( a );
    asset()->release( b );
    printf( "  after 2 releases: refcount=%d state=%s count=%u\n",
            asset()->refcount( c ), state_name( asset()->state( c ) ), asset()->count() );
    asset()->release( c );
    printf( "  after 3 releases: count=%u unloads=%d  a.valid=%d (stale handle rejected)\n",
            asset()->count(), g_blob_unloads, asset()->valid( a ) );

    /* 5) missing file -> FAILED but releasable handle. */
    asset_id_t m = asset()->acquire( "data/nope.blob" );
    printf( "  missing acquire: valid=%d state=%s get=%p\n",
            asset()->valid( m ), state_name( asset()->state( m ) ), asset()->get( m ) );
    asset()->release( m );
    printf( "  final count=%u\n", asset()->count() );

    sys_file_delete( probe_real );
}

/*==============================================================================================
    Phase 4 proof -- hot-reload via mtime poll (asset()->refresh), no GPU.

    Acquire a blob, prove refresh() is a no-op while the source is unchanged, then rewrite the
    file and prove refresh() re-runs the loader in place: same id + refcount, fresh bytes.
==============================================================================================*/

static void
asset_refresh_test( void )
{
    printf( "\n=== asset service (hot-reload / refresh) ===\n" );

    const char* probe_real = "asset_reload.rlb";        // served through the "data/" mount below
    const char* v1         = "reload payload VERSION ONE";
    const char* v2         = "reload payload -- VERSION TWO (longer)";

    if ( !sys_file_write_entire( probe_real, v1, ( u32 )strlen( v1 ) ) )
    {
        printf( "  FAIL: could not write probe file\n" );
        return;
    }
    fs()->mount( "data/", "", 0 );

    const char* exts[] = { ".rlb" };
    asset()->type_register( "blob", exts, 1, blob_load, blob_unload, NULL );

    int        loads0 = g_blob_loads;
    asset_id_t a      = asset()->acquire( "data/asset_reload.rlb" );
    blob_res_t* r1    = ( blob_res_t* )asset()->get( a );
    printf( "  acquire: id={%u,%u} state=%s v1-match=%d loads=%d\n",
            a.index, a.generation, state_name( asset()->state( a ) ),
            r1 && strcmp( r1->bytes, v1 ) == 0, g_blob_loads - loads0 );

    /* 1) unchanged source -> refresh reloads nothing. */
    u32 n_noop = asset()->refresh();
    printf( "  refresh (unchanged): reloaded=%u loads=%d (expect 0 / no new load)\n",
            n_noop, g_blob_loads - loads0 );

    /* 2) rewrite the source (new bytes + a guaranteed-newer mtime), then refresh reloads it in
          place -- same id and refcount, fresh content.  Sleep past Windows' ~15ms file-time
          granularity so the mtime is certain to differ. */
    sys_sleep_milliseconds( 40 );
    sys_file_write_entire( probe_real, v2, ( u32 )strlen( v2 ) );

    u32         n_hot = asset()->refresh();
    asset_id_t  a2    = a;                              // id is unchanged by an in-place reload
    blob_res_t* r2    = ( blob_res_t* )asset()->get( a2 );
    printf( "  refresh (rewritten): reloaded=%u loads=%d id-same=%d refcount=%d v2-match=%d\n",
            n_hot, g_blob_loads - loads0,
            ( a2.index == a.index && a2.generation == a.generation ),
            asset()->refcount( a2 ), r2 && strcmp( r2->bytes, v2 ) == 0 );

    asset()->release( a );
    sys_file_delete( probe_real );
    printf( "  final count=%u unloads accounted\n", asset()->count() );
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( fs );       // virtual filesystem: leaf on sys, asset depends on it
    mod_static( app );
    mod_static( core );
    mod_static( rhi );      // asset now depends on rhi (image loader); registered but not init'd here
    mod_static( asset );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    asset_test();
    asset_refresh_test();

    mod_system_exit();
    return 0;
}

/*============================================================================================*/
