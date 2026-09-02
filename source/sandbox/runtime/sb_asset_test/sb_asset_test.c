/*==============================================================================================

    sandbox/runtime/sb_asset_test/sb_asset_test.c -- asset service registry proof.

    Boots sys + ref + core + asset through the module system, serves the working directory as
    content beneath "sandbox/asset/", registers a trivial "blob" asset type, then exercises the
    registry by name:
      - acquire the same name twice -> same handle, refcount 2  (dedup)
      - get() returns the decoded resource; state == LOADED; name() reads back
      - the type's extensions are tried in preference order
      - a name that is not canonical (uppercase, backslash) -> invalid handle, nothing allocated
      - the same name acquired as another type -> invalid handle
      - release down to zero -> unloaded, count back to 0, stale handle rejected
      - a name with no file under any extension -> FAILED (but releasable) handle
      - refresh(): no-op while unchanged, reload in place after a rewrite, and a FAILED
        record loads once its file appears

    The names are plain strings, not RID() markers: this sandbox writes its files as it runs,
    so no marked literal could resolve against content/ at build time.  No renderer here; the
    "blob" loader just copies bytes into a small heap struct.

    Exit code = number of failed checks.

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
#include "engine/pack/pack_host.h"
#include "engine/fs/fs_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/asset/asset_host.h"

/*==============================================================================================
    Checks
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
check( bool ok, const char* what )
{
    ++s_checks;
    if ( !ok )
        ++s_fails;
    printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what );
}

/*==============================================================================================
    Trivial "blob" asset type -- proves the type_register / load / unload seam with no GPU.
==============================================================================================*/

typedef struct blob_res_s
{
    u32  size;
    char bytes[ 1 ];    // flexible-ish tail; allocated size = sizeof(u32) + size + 1

} blob_res_t;

static int  g_blob_loads   = 0;    // instrumentation: how many times the loader ran
static int  g_blob_unloads = 0;
static char g_blob_last_path[ ASSET_PATH_MAX ];    // the vpath the loader was handed last

static void*
blob_load( const char* path, const void* data, u32 size, void* userdata )
{
    UNUSED( userdata );

    snprintf( g_blob_last_path, sizeof( g_blob_last_path ), "%s", path );

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
    Registry
==============================================================================================*/

static void
asset_test( void )
{
    printf( "\n=== asset service (registry) ===\n" );

    const char* payload = "orb asset blob payload";
    u32         plen    = ( u32 )strlen( payload );
    if ( !sys_file_write_entire( "probe.tmp", payload, plen ) )
    {
        check( false, "write the probe file" );
        return;
    }

    /* The type prefers .blob and falls back to .tmp; the probe exists only as .tmp. */
    const char* exts[] = { ".blob", ".tmp" };
    u16         type   = asset()->type_register( "blob", exts, 2, blob_load, blob_unload, NULL );
    check( type >= 1, "blob type registered" );

    /* 1) dedup: the same name twice shares one record, refcount climbs to 2. */
    aid_t a = asset()->acquire( "sandbox/asset/probe", type );
    aid_t b = asset()->acquire( "sandbox/asset/probe", type );
    check( a.index != 0 && a.index == b.index && a.generation == b.generation, "same name twice -> one handle" );
    check( asset()->refcount( a ) == 2 && asset()->count() == 1 && g_blob_loads == 1,
           "refcount 2, one record, one load" );
    check( asset()->name( a ) && strcmp( asset()->name( a ), "sandbox/asset/probe" ) == 0,
           "name() reads back the resource" );

    /* 2) state + get: resource decoded once, readable, byte-identical. */
    blob_res_t* r = ( blob_res_t* )asset()->get( a );
    check( asset()->state( a ) == ASSET_LOADED && r && r->size == plen && memcmp( r->bytes, payload, plen ) == 0,
           "LOADED and the bytes match" );

    /* 3) extension preference: .blob is absent, so the second choice loaded. */
    check( strcmp( g_blob_last_path, "sandbox/asset/probe.tmp" ) == 0,
           "the second extension is used when the first is absent" );

    /* 4) spelling: names are canonical and nothing folds, so another spelling is refused
          outright rather than probed against fs or filed as a second record. */
    u32   n  = asset()->count();
    aid_t c1 = asset()->acquire( "Sandbox/Asset/PROBE", type );
    aid_t c2 = asset()->acquire( "sandbox\\asset\\probe", type );
    check( c1.index == 0 && c2.index == 0 && asset()->count() == n && g_blob_loads == 1,
           "a non-canonical spelling is refused, nothing allocated" );
    check( asset()->acquire( "", type ).index == 0 && asset()->acquire( NULL, type ).index == 0,
           "an empty or NULL name is refused" );

    /* 5) one resource, one type: the same name as another type is refused. */
    const char* other_exts[] = { ".tmp" };
    u16         other        = asset()->type_register( "other", other_exts, 1, blob_load, blob_unload, NULL );
    aid_t       t2           = asset()->acquire( "sandbox/asset/probe", other );
    check( t2.index == 0 && asset()->refcount( a ) == 2, "the same name as another type is refused" );

    /* 6) preference the other way: both files present, the first extension wins. */
    sys_file_write_entire( "pair.blob", "B", 1 );
    sys_file_write_entire( "pair.tmp", "T", 1 );
    aid_t       p  = asset()->acquire( "sandbox/asset/pair", type );
    blob_res_t* pr = ( blob_res_t* )asset()->get( p );
    check( pr && pr->bytes[ 0 ] == 'B' && strcmp( g_blob_last_path, "sandbox/asset/pair.blob" ) == 0,
           "the first extension wins when both files exist" );
    asset()->release( p );
    sys_file_delete( "pair.blob" );
    sys_file_delete( "pair.tmp" );

    /* 7) release down to zero: two acquires (a, b) need two releases. */
    asset()->release( a );
    check( asset()->refcount( b ) == 1 && asset()->state( b ) == ASSET_LOADED,
           "one release leaves one reference, still LOADED" );
    asset()->release( b );
    check( asset()->count() == 0 && g_blob_unloads == 2 && !asset()->valid( a ),
           "the last release unloads; a stale handle is rejected" );
    check( asset()->name( a ) == NULL, "a stale handle has no name" );

    /* 8) a name with no file under any extension: FAILED but releasable. */
    aid_t m = asset()->acquire( "sandbox/asset/nope", type );
    check( asset()->valid( m ) && asset()->state( m ) == ASSET_FAILED && asset()->get( m ) == NULL,
           "a name with no file is FAILED but releasable" );
    asset()->release( m );
    check( asset()->count() == 0, "count back to zero" );

    sys_file_delete( "probe.tmp" );
}

/*==============================================================================================
    Hot reload via mtime poll (asset()->refresh), no GPU.

    Acquire a blob, prove refresh() is a no-op while the source is unchanged, then rewrite the
    file and prove refresh() re-runs the loader in place: same id + refcount, fresh bytes.  A
    FAILED record is retried too: its file appearing later is enough.
==============================================================================================*/

static void
asset_refresh_test( void )
{
    printf( "\n=== asset service (hot-reload / refresh) ===\n" );

    const char* v1 = "reload payload VERSION ONE";
    const char* v2 = "reload payload -- VERSION TWO (longer)";
    if ( !sys_file_write_entire( "reload.rlb", v1, ( u32 )strlen( v1 ) ) )
    {
        check( false, "write the reload file" );
        return;
    }

    const char* exts[] = { ".rlb" };
    u16         type   = asset()->type_register( "reload", exts, 1, blob_load, blob_unload, NULL );

    int         loads0 = g_blob_loads;
    aid_t       a      = asset()->acquire( "sandbox/asset/reload", type );
    blob_res_t* r1     = ( blob_res_t* )asset()->get( a );
    check( asset()->state( a ) == ASSET_LOADED && r1 && strcmp( r1->bytes, v1 ) == 0 && g_blob_loads - loads0 == 1,
           "acquired, version one" );

    /* 1) unchanged source -> refresh reloads nothing. */
    u32 n_noop = asset()->refresh();
    check( n_noop == 0 && g_blob_loads - loads0 == 1, "refresh on an unchanged file reloads nothing" );

    /* 2) rewrite the source (new bytes + a guaranteed-newer mtime), then refresh reloads it in
          place -- same id and refcount, fresh content.  Sleep past Windows' ~15ms file-time
          granularity so the mtime is certain to differ. */
    sys_sleep_milliseconds( 40 );
    sys_file_write_entire( "reload.rlb", v2, ( u32 )strlen( v2 ) );

    u32         n_hot = asset()->refresh();
    blob_res_t* r2    = ( blob_res_t* )asset()->get( a );
    check( n_hot == 1 && asset()->valid( a ) && asset()->refcount( a ) == 1 && r2 && strcmp( r2->bytes, v2 ) == 0,
           "refresh after a rewrite reloads in place: same id, version two" );

    /* 3) a FAILED record loads once its file appears. */
    aid_t l = asset()->acquire( "sandbox/asset/late", type );
    check( asset()->state( l ) == ASSET_FAILED, "a missing file is FAILED at acquire" );
    sys_file_write_entire( "late.rlb", "late", 4 );
    u32 n_late = asset()->refresh();
    check( n_late == 1 && asset()->state( l ) == ASSET_LOADED, "refresh loads a FAILED record whose file appeared" );

    asset()->release( l );
    asset()->release( a );
    sys_file_delete( "reload.rlb" );
    sys_file_delete( "late.rlb" );
    check( asset()->count() == 0, "count back to zero" );
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
    mod_static( pack );     // compression: leaf, fs zip mounts go through it
    mod_static( fs );       // virtual filesystem: leaf on sys+pack, asset depends on it
    mod_static( app );
    mod_static( core );
    mod_static( rhi );      // asset depends on rhi (image loader); registered but not init'd here
    mod_static( asset );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    /* The working directory stands in for a content tree: "sandbox/asset/<name>.<ext>" maps
       to "./<name>.<ext>", the shape every test above writes. */
    fs()->mount( "sandbox/asset/", "", 0 );

    asset_test();
    asset_refresh_test();

    printf( "\nsb_asset_test: %d checks, %d failed\n", s_checks, s_fails );
    mod_system_exit();
    return s_fails;
}

/*============================================================================================*/
