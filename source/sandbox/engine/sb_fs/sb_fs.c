/*==============================================================================================

    sandbox/engine/sb_fs/sb_fs.c -- testbed for the virtual filesystem (engine/fs).

    Not a real host; a headless place to exercise the fs library and verify it works.  Covers
    the DIR read path, the ZIP bundle reader, loose-over-bundle priority + hot-reload, glob,
    unmount, and the fs() module gateway.  Every scratch file/dir is built here and cleaned up,
    so the suite leaves nothing committed and needs no fixtures.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "orb.h"
#include "engine/mod/mod_host.h"    // module-gateway test: mod_static / mod_init_all
#include "engine/sys/sys_host.h"    // sys_file_* / sys_dir_make for scratch fixtures
#include "engine/fs/fs_host.h"      // the library under test (direct calls + fs() gateway)
#include "engine/pack/pack_host.h"  // pack_zip_writer_* to build a test bundle in memory

/*==============================================================================================
    Check helper (matches sb_net / sb_prof house style)
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
sb_check( bool ok, const char* what )
{
    s_checks++;
    if ( !ok )
    {
        s_fails++;
        printf( "    FAIL: %s\n", what );
    }
}

/* True if the blob loaded OK and its bytes exactly equal `text` (excludes the hidden NUL). */
static bool
blob_is( const fs_blob_t* b, const char* text )
{
    u32 n = ( u32 )strlen( text );
    return b->ok && b->size == n && memcmp( b->data, text, n ) == 0;
}

/*==============================================================================================
    DIR mounts -- mount a real directory and read a file back through a virtual path.
==============================================================================================*/

static void
fs_test_dir( void )
{
    printf( "  -- dir mount, read, catalog, normalization --\n" );

    fs_system_init();

    const char* probe_real = "sb_fs_probe.tmp";              // in the current working dir
    const char* payload    = "orb vfs dir-mount probe\n";
    u32         plen       = ( u32 )strlen( payload );
    sb_check( sys_file_write_entire( probe_real, payload, plen ), "write probe file" );

    /* Map the virtual prefix "data/" onto the current working directory. */
    sb_check( fs_mount( "data/", "", 0 ), "mount data/ -> CWD" );

    /* exists + stat through the vpath (no bytes read yet). */
    fs_stat_t st = { 0 };
    sb_check( fs_exists( "data/sb_fs_probe.tmp" ), "exists via vpath" );
    sb_check( fs_stat( "data/sb_fs_probe.tmp", &st ) && st.ok, "stat ok" );
    sb_check( st.size == plen, "stat size == payload size" );

    /* read + byte-compare; the blob carries a hidden trailing NUL. */
    fs_blob_t b = fs_read( "data/sb_fs_probe.tmp" );
    sb_check( blob_is( &b, payload ), "read bytes match payload" );
    sb_check( ( ( const char* )b.data )[ b.size ] == '\0', "blob is NUL-terminated" );
    fs_free( &b );

    /* second read is served from the catalog; one file cached. */
    fs_blob_t b2 = fs_read( "data/sb_fs_probe.tmp" );
    sb_check( b2.ok, "second (cached) read ok" );
    sb_check( fs_file_count() == 1, "catalog holds one entry" );
    fs_free( &b2 );

    /* missing path resolves to nothing (and a failed blob is safe to free). */
    sb_check( !fs_exists( "data/does_not_exist.xyz" ), "missing path does not exist" );
    fs_blob_t miss = fs_read( "data/does_not_exist.xyz" );
    sb_check( !miss.ok && miss.data == NULL, "missing read returns failed blob" );
    fs_free( &miss );

    /* backslashes + case fold to the same file (case-insensitive vpath, Win FS). */
    sb_check( fs_exists( "Data\\SB_FS_PROBE.TMP" ), "backslash + case alt-form resolves" );

    sys_file_delete( probe_real );
    fs_system_exit();
}

/*==============================================================================================
    ZIP mounts -- read entries out of a .zip bundle (central-dir parse + inflate).
==============================================================================================*/

/* Build a three-file zip (docs/readme.txt, shared.txt, late.txt) in memory, write to `path`. */
static bool
build_test_zip( const char* path )
{
    const char* readme = "orb vfs -- readme served from a zip bundle";
    const char* shared = "FROM ZIP";
    const char* late   = "FROM ZIP LATE";

    pack_zip_writer_t* zw = pack_zip_writer_begin();
    if ( !zw )
        return false;

    /* PACK_LEVEL_BEST -> payloads are DEFLATE'd, so reads exercise the inflate path. */
    pack_zip_writer_add( zw, "docs/readme.txt", readme, ( u32 )strlen( readme ), PACK_LEVEL_BEST );
    pack_zip_writer_add( zw, "shared.txt", shared, ( u32 )strlen( shared ), PACK_LEVEL_BEST );
    pack_zip_writer_add( zw, "late.txt", late, ( u32 )strlen( late ), PACK_LEVEL_BEST );

    void* buf = NULL;
    u32   sz  = 0;
    bool  ok  = pack_zip_writer_end( zw, &buf, &sz );
    if ( ok )
        ok = sys_file_write_entire( path, buf, sz );

    free( buf );    // writer_end transfers ownership of the heap block to us
    return ok;
}

static void
fs_test_zip( const char* zip_path )
{
    printf( "  -- zip bundle read + stat stability --\n" );

    fs_system_init();
    sb_check( fs_mount( "pak/", zip_path, 0 ), "mount zip pak/ -> .zip" );

    /* nested, deflated entry. */
    fs_blob_t rd = fs_read( "pak/docs/readme.txt" );
    sb_check( blob_is( &rd, "orb vfs -- readme served from a zip bundle" ), "read nested deflated entry" );
    fs_free( &rd );

    fs_blob_t sh = fs_read( "pak/shared.txt" );
    sb_check( blob_is( &sh, "FROM ZIP" ), "read top-level entry" );
    fs_free( &sh );

    /* zip entries carry the bundle's stable mtime -> two stats agree (no spurious reload). */
    fs_stat_t z1 = { 0 }, z2 = { 0 };
    sb_check( fs_stat( "pak/docs/readme.txt", &z1 ), "zip stat ok" );
    fs_stat( "pak/docs/readme.txt", &z2 );
    sb_check( z1.mtime == z2.mtime && z1.mtime != 0, "zip mtime stable + nonzero" );

    sb_check( !fs_exists( "pak/nope.txt" ), "missing zip entry does not exist" );

    fs_system_exit();
}

/*==============================================================================================
    Loose-over-bundle -- a higher-priority DIR mount shadows a ZIP entry, stays hot-reloadable,
    and evicts back to the bundle when the loose override vanishes.
==============================================================================================*/

static void
fs_test_shadow( const char* zip_path )
{
    printf( "  -- loose-over-bundle priority + hot-reload --\n" );

    const char* shadow_rel = "shared.txt";    // loose file in CWD; DIR mount "pak/"->"" serves it
    sys_file_write_entire( shadow_rel, "FROM LOOSE", 10 );

    fs_system_init();
    fs_mount( "pak/", zip_path, 0 );           // bundle at low priority
    fs_mount( "pak/", "", 10 );                // loose CWD at high priority -> shadows the bundle

    fs_blob_t win = fs_read( "pak/shared.txt" );
    sb_check( blob_is( &win, "FROM LOOSE" ), "loose file shadows bundle entry" );
    fs_free( &win );

    /* the shadowing loose file is DIR-backed, so fs_stat re-stats live -> still hot-reloadable. */
    fs_stat_t s1 = { 0 }, s2 = { 0 };
    fs_stat( "pak/shared.txt", &s1 );
    sys_sleep_milliseconds( 40 );              // clear Windows' ~15ms file-time granularity
    sys_file_write_entire( shadow_rel, "FROM LOOSE v2", 13 );
    fs_stat( "pak/shared.txt", &s2 );
    sb_check( s1.mtime != s2.mtime, "loose shadow mtime changes (hot-reloadable)" );

    /* a path only in the bundle still resolves through the shadow mount. */
    fs_blob_t only = fs_read( "pak/docs/readme.txt" );
    sb_check( blob_is( &only, "orb vfs -- readme served from a zip bundle" ),
              "bundle still serves non-shadowed path" );
    fs_free( &only );

    /* Late shadow: a loose override dropped AFTER the path was cataloged from the bundle.
       fs_stat re-resolves when a DIR mount sits above the cached winner, so the new file is
       picked up on the next stat; deleting it falls back to the bundle (evict-on-vanish). */
    fs_blob_t pre = fs_read( "pak/late.txt" );    // catalogs the ZIP winner
    sb_check( blob_is( &pre, "FROM ZIP LATE" ), "late path starts from bundle" );
    fs_free( &pre );

    fs_stat_t l1 = { 0 }, l2 = { 0 };
    fs_stat( "pak/late.txt", &l1 );
    sys_file_write_entire( "late.txt", "FROM LOOSE LATE", 15 );
    fs_stat( "pak/late.txt", &l2 );
    sb_check( l1.mtime != l2.mtime, "late-shadow stat re-resolves to loose" );

    fs_blob_t post = fs_read( "pak/late.txt" );
    sb_check( blob_is( &post, "FROM LOOSE LATE" ), "late loose override now wins" );
    fs_free( &post );

    sys_file_delete( "late.txt" );
    fs_blob_t back = fs_read( "pak/late.txt" );
    sb_check( blob_is( &back, "FROM ZIP LATE" ), "evict-on-vanish falls back to bundle" );
    fs_free( &back );

    fs_system_exit();
    sys_file_delete( shadow_rel );
}

/*==============================================================================================
    Priority + unmount -- highest-priority mount that HAS the file wins; unmount drops it and
    re-resolves down the stack.
==============================================================================================*/

static void
fs_test_priority( void )
{
    printf( "  -- multi-mount priority + unmount --\n" );

    sys_dir_make( "sb_fs_lo" );
    sys_dir_make( "sb_fs_hi" );
    sys_file_write_entire( "sb_fs_lo/pick.txt", "LOW", 3 );
    sys_file_write_entire( "sb_fs_hi/pick.txt", "HIGH", 4 );
    sys_file_write_entire( "sb_fs_lo/only_lo.txt", "ONLY LOW", 8 );

    fs_system_init();
    fs_mount( "res/", "sb_fs_lo/", 0 );        // low priority
    fs_mount( "res/", "sb_fs_hi/", 10 );       // high priority

    fs_blob_t hi = fs_read( "res/pick.txt" );
    sb_check( blob_is( &hi, "HIGH" ), "higher-priority mount wins collision" );
    fs_free( &hi );

    /* a file only the low mount has still resolves (priority breaks ties, not coverage). */
    fs_blob_t lo = fs_read( "res/only_lo.txt" );
    sb_check( blob_is( &lo, "ONLY LOW" ), "lower mount serves its exclusive file" );
    fs_free( &lo );

    /* unmount drops every mount matching the prefix and clears the catalog, so the vpath that
       resolved a moment ago now resolves to nothing. */
    fs_unmount( "res/" );
    fs_blob_t after = fs_read( "res/pick.txt" );
    sb_check( !after.ok, "after unmount res/, vpath no longer resolves" );
    fs_free( &after );

    fs_system_exit();

    sys_file_delete( "sb_fs_lo/pick.txt" );
    sys_file_delete( "sb_fs_lo/only_lo.txt" );
    sys_file_delete( "sb_fs_hi/pick.txt" );
}

/*==============================================================================================
    Glob -- pattern match over DIR mounts (ZIP entries are not enumerated).
==============================================================================================*/

typedef struct glob_ctx_s
{
    int  count;
    bool saw_alpha;
    bool saw_other_ext;

} glob_ctx_t;

static bool
glob_cb( const char* vpath, void* userdata )
{
    glob_ctx_t* c = ( glob_ctx_t* )userdata;
    c->count++;
    if ( strstr( vpath, "alpha.gtxt" ) )
        c->saw_alpha = true;
    if ( strstr( vpath, ".other" ) )
        c->saw_other_ext = true;
    return true;    // keep iterating
}

static void
fs_test_glob( void )
{
    printf( "  -- glob over a dir mount --\n" );

    sys_dir_make( "sb_fs_glob" );
    sys_file_write_entire( "sb_fs_glob/alpha.gtxt", "a", 1 );
    sys_file_write_entire( "sb_fs_glob/beta.gtxt", "b", 1 );
    sys_file_write_entire( "sb_fs_glob/gamma.other", "g", 1 );

    fs_system_init();
    fs_mount( "g/", "sb_fs_glob/", 0 );

    glob_ctx_t c = { 0 };
    int        n = fs_glob( "g/*.gtxt", glob_cb, &c );
    sb_check( n == 2, "glob '*.gtxt' returns 2 matches" );
    sb_check( c.count == 2, "callback fired for each match" );
    sb_check( c.saw_alpha, "callback receives the virtual path" );
    sb_check( !c.saw_other_ext, "non-matching extension excluded" );

    fs_system_exit();

    sys_file_delete( "sb_fs_glob/alpha.gtxt" );
    sys_file_delete( "sb_fs_glob/beta.gtxt" );
    sys_file_delete( "sb_fs_glob/gamma.other" );
}

/*==============================================================================================
    Module gateway -- fs is a leaf module (deps: sys).  Register it through the module system
    and drive it via the fs() gateway, proving fs_get_mod_desc + fs_api_t wire up correctly.
==============================================================================================*/

static void
fs_test_module( void )
{
    printf( "  -- fs() module gateway --\n" );

    mod_system_init();
    bool loaded = mod_static( sys ) && mod_static( pack ) && mod_static( fs );    // fs deps sys + pack
    sb_check( loaded, "register sys + pack + fs as static modules" );
    sb_check( mod_init_all(), "mod_init_all (runs fs_system_init via fs_mod_init)" );

    /* fs() resolves to the static gateway here (sb_fs declares 'dep fs' -> FS_STATIC). */
    const char* payload = "through the gateway";
    sys_file_write_entire( "sb_fs_gw.tmp", payload, ( u32 )strlen( payload ) );
    sb_check( fs()->mount( "gw/", "", 0 ), "fs()->mount" );

    fs_blob_t b = fs()->read( "gw/sb_fs_gw.tmp" );
    sb_check( blob_is( &b, payload ), "fs()->read returns the bytes" );
    fs()->free( &b );

    sys_file_delete( "sb_fs_gw.tmp" );
    mod_system_exit();    // fs_mod_exit -> fs_system_exit
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    const char* zip_path = "sb_fs_pak.zip";

    printf( "========================================\n" );
    printf( " fs -- dir mounts\n" );
    printf( "========================================\n" );
    fs_test_dir();

    printf( "\n========================================\n" );
    printf( " fs -- zip bundles + loose-over-bundle\n" );
    printf( "========================================\n" );
    if ( build_test_zip( zip_path ) )
    {
        fs_test_zip( zip_path );
        fs_test_shadow( zip_path );
        sys_file_delete( zip_path );
    }
    else
    {
        sb_check( false, "build in-memory test zip" );
    }

    printf( "\n========================================\n" );
    printf( " fs -- priority, glob, gateway\n" );
    printf( "========================================\n" );
    fs_test_priority();
    fs_test_glob();
    fs_test_module();

    printf( "\n%d checks, %d failures\n", s_checks, s_fails );
    printf( s_fails == 0 ? "ALL PASS\n" : "FAILED\n" );
    return s_fails == 0 ? 0 : 1;
}

/*============================================================================================*/
