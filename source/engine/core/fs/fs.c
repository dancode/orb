/*==============================================================================================

    engine/core/fs/fs.c -- virtual filesystem implementation (DIR mounts + lazy catalog).

    Resolution: normalize the vpath, then among mounts whose vprefix matches, pick the
    highest-priority mount that actually has the file (loose-over-bundle).  The first hit is
    cached in the catalog so repeat lookups are O(1) and never re-scan the mount table.

==============================================================================================*/

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#include "orb.h"
#include "engine/core/fs/fs.h"
#include "engine/core/fs/fs_zip.h"   /* miniz config -- must precede vendor/miniz.h */
#include "vendor/miniz.h"            /* mz_zip_* reader (impl in fs_zip_miniz.c) */
#include "engine/core/sid/sid.h"     /* sid_hash_len -- case-insensitive FNV-1a */
#include "engine/sys/sys_host.h"     /* sys_file_read_entire / _exists / _size / _time / _glob */

/*==============================================================================================
    State
==============================================================================================*/

/* A mount is either a real directory (DIR) or a .zip archive (ZIP).  Both resolve through the
   same vprefix/priority logic; only the backing read differs.  loose-over-bundle falls out of
   the shared priority scan -- a DIR file at higher priority shadows a ZIP entry for free. */
typedef enum fs_mount_kind_e
{
    FS_MOUNT_DIR = 0,   // real names a directory on disk
    FS_MOUNT_ZIP,       // real names a .zip; entries served via miniz

} fs_mount_kind_t;

typedef struct fs_mount_s
{
    char            vprefix[ FS_PATH_MAX ];   // normalized, trailing '/' (or "" = match all)
    char            real[ FS_PATH_MAX ];      // DIR: dir with trailing '/'.  ZIP: the .zip path.
    int             priority;
    u8              kind;                     // fs_mount_kind_t
    bool            used;

    /* ZIP mounts only: the whole archive is read into memory once (sys owns disk I/O) and kept
       alive for the mount's lifetime so the reader can extract by name on demand.  Every entry
       reports the .zip's own mtime -- a bundle is immutable in place, so this is stable and
       hot-reload never re-runs a loader for a zip-backed asset (a loose shadow does that). */
    mz_zip_archive* zip;
    void*           zip_bytes;
    u64             zip_mtime;

} fs_mount_t;

typedef struct fs_entry_s
{
    u32  hash;                     // sid_hash_len of vpath
    u16  mount;                    // owning mount index
    u16  used;
    u32  size;
    u64  mtime;
    char vpath[ FS_PATH_MAX ];     // full virtual path (catalog key)
    char real[ FS_PATH_MAX ];      // resolved real OS path (avoids re-resolve on read)

} fs_entry_t;

static fs_mount_t  s_mounts[ FS_MAX_MOUNTS ];
static u32         s_mount_count;
static fs_entry_t* s_catalog;        // FS_MAX_FILES entries, calloc'd in init
static u32         s_catalog_count;

/*==============================================================================================
    Path helpers
==============================================================================================*/

/* Copy `in` to `out`, converting '\\' -> '/' and stripping leading slashes. For lookups. */
static void
fs_norm_vpath( char* out, const char* in )
{
    while ( *in == '/' || *in == '\\' )
        ++in;

    u32 n = 0;
    for ( ; in[ 0 ] && n < FS_PATH_MAX - 1; ++in )
        out[ n++ ] = ( *in == '\\' ) ? '/' : *in;
    out[ n ] = '\0';
}

/* Copy `in` to `out`, converting '\\' -> '/' and ensuring a trailing '/' (unless empty). */
static void
fs_norm_dir( char* out, const char* in )
{
    u32 n = 0;
    for ( ; in[ 0 ] && n < FS_PATH_MAX - 2; ++in )
        out[ n++ ] = ( *in == '\\' ) ? '/' : *in;

    if ( n > 0 && out[ n - 1 ] != '/' )
        out[ n++ ] = '/';
    out[ n ] = '\0';
}

/* Case-insensitive full-string compare. */
static bool
fs_ci_eq( const char* a, const char* b )
{
    for ( ;; )
    {
        int ca = tolower( ( unsigned char )*a );
        int cb = tolower( ( unsigned char )*b );
        if ( ca != cb )
            return false;
        if ( ca == '\0' )
            return true;
        ++a;
        ++b;
    }
}

/* True if `vpath` starts with the mount prefix `pfx` (case-insensitive). "" matches all. */
static bool
fs_prefix_match( const char* vpath, const char* pfx )
{
    for ( ; *pfx; ++pfx, ++vpath )
    {
        if ( tolower( ( unsigned char )*vpath ) != tolower( ( unsigned char )*pfx ) )
            return false;
    }
    return true;
}

/* Build the real OS path for `vpath` under mount `m` (vpath already prefix-matched). */
static bool
fs_build_real( const fs_mount_t* m, const char* vpath, char* out )
{
    const char* rel = vpath + strlen( m->vprefix );
    while ( *rel == '/' )
        ++rel;
    int n = snprintf( out, FS_PATH_MAX, "%s%s", m->real, rel );
    return n > 0 && n < FS_PATH_MAX;
}

/*==============================================================================================
    ZIP backing  (miniz reader over an in-memory archive)
==============================================================================================*/

/* True if `path` ends in ".zip" (case-insensitive) -- how fs_mount tells a bundle from a dir. */
static bool
fs_has_zip_ext( const char* path )
{
    size_t n = strlen( path );
    return n >= 4 && fs_ci_eq( path + n - 4, ".zip" );
}

/* In-archive entry name for `vpath` under a ZIP mount: the vpath beyond the mount prefix, with
   any leading slashes stripped (zip central-dir names carry no leading '/').  Written to `out`. */
static void
fs_zip_rel( const fs_mount_t* m, const char* vpath, char* out )
{
    const char* rel = vpath + strlen( m->vprefix );
    while ( *rel == '/' )
        ++rel;
    snprintf( out, FS_PATH_MAX, "%s", rel );
}

/* Locate `rel` in the mount's archive; returns the file index or -1.  Case-insensitive (flags
   0), matching the vfs's case-folding elsewhere. */
static int
fs_zip_locate( const fs_mount_t* m, const char* rel )
{
    if ( !m->zip || !rel[ 0 ] )
        return -1;
    return mz_zip_reader_locate_file( m->zip, rel, NULL, 0 );
}

/* Size (and the mount's stable mtime) for a ZIP entry named `rel`.  Returns false if absent. */
static bool
fs_zip_meta( const fs_mount_t* m, const char* rel, u32* out_size, u64* out_mtime )
{
    int idx = fs_zip_locate( m, rel );
    if ( idx < 0 )
        return false;

    mz_zip_archive_file_stat st;
    if ( !mz_zip_reader_file_stat( m->zip, ( mz_uint )idx, &st ) )
        return false;

    if ( out_size )
        *out_size = ( u32 )st.m_uncomp_size;
    if ( out_mtime )
        *out_mtime = m->zip_mtime;    // per-entry time is not tracked; the bundle's is stable
    return true;
}

/* Extract ZIP entry `rel` into a fresh malloc'd buffer (freed by fs_free like any blob).  The
   buffer carries a hidden trailing NUL past `*out_size`, matching the fs_blob_t contract so text
   payloads work as C strings.  Returns NULL if the entry is absent or decompression fails. */
static void*
fs_zip_read( const fs_mount_t* m, const char* rel, u32* out_size )
{
    int idx = fs_zip_locate( m, rel );
    if ( idx < 0 )
        return NULL;

    mz_zip_archive_file_stat st;
    if ( !mz_zip_reader_file_stat( m->zip, ( mz_uint )idx, &st ) )
        return NULL;

    size_t size = ( size_t )st.m_uncomp_size;
    char*  buf  = ( char* )malloc( size + 1 );
    if ( !buf )
        return NULL;

    if ( size && !mz_zip_reader_extract_to_mem( m->zip, ( mz_uint )idx, buf, size, 0 ) )
    {
        free( buf );
        return NULL;
    }
    buf[ size ] = '\0';
    if ( out_size )
        *out_size = ( u32 )size;
    return buf;
}

/* size + mtime for a resolved (mount, real) pair, dispatched by mount kind.  `real` is an OS
   path for DIR, an in-archive name for ZIP.  Returns false if the backing is gone. */
static bool
fs_entry_meta( const fs_mount_t* m, const char* real, u32* out_size, u64* out_mtime )
{
    if ( m->kind == FS_MOUNT_ZIP )
        return fs_zip_meta( m, real, out_size, out_mtime );

    u64 mtime = sys_file_time( real );
    if ( mtime == 0 )
        return false;    // vanished from disk
    if ( out_size )
        *out_size = sys_file_size( real );
    if ( out_mtime )
        *out_mtime = mtime;
    return true;
}

/*==============================================================================================
    Catalog
==============================================================================================*/

static fs_entry_t*
fs_catalog_find( const char* vpath )
{
    if ( !s_catalog )
        return NULL;

    u32 hash = sid_hash_len( vpath, strlen( vpath ) );
    u32 mask = FS_MAX_FILES - 1;
    u32 i    = hash & mask;

    for ( u32 n = 0; n < FS_MAX_FILES; ++n )
    {
        fs_entry_t* e = &s_catalog[ i ];
        if ( !e->used )
            return NULL;    // empty slot: not present (lazy fill never deletes, so no tombstones)
        if ( e->hash == hash && fs_ci_eq( e->vpath, vpath ) )
            return e;
        i = ( i + 1 ) & mask;
    }
    return NULL;
}

static fs_entry_t*
fs_catalog_insert( const char* vpath, const char* real, u16 mount, u32 size, u64 mtime )
{
    if ( !s_catalog )
        return NULL;
    if ( s_catalog_count >= ( FS_MAX_FILES * 3 ) / 4 )
        return NULL;    // keep the load factor sane; reads still work uncached

    u32 hash = sid_hash_len( vpath, strlen( vpath ) );
    u32 mask = FS_MAX_FILES - 1;
    u32 i    = hash & mask;

    for ( u32 n = 0; n < FS_MAX_FILES; ++n )
    {
        fs_entry_t* e = &s_catalog[ i ];
        if ( !e->used )
        {
            e->used  = 1;
            e->hash  = hash;
            e->mount = mount;
            e->size  = size;
            e->mtime = mtime;
            snprintf( e->vpath, FS_PATH_MAX, "%s", vpath );
            snprintf( e->real, FS_PATH_MAX, "%s", real );
            ++s_catalog_count;
            return e;
        }
        if ( e->hash == hash && fs_ci_eq( e->vpath, vpath ) )
            return e;    // already cached
        i = ( i + 1 ) & mask;
    }
    return NULL;
}

/*==============================================================================================
    Resolution
==============================================================================================*/

/* Find the best mount for `vpath` (highest priority that actually has the file) and fill
   `real_out` with the resolved OS path. Returns the mount, or NULL if the file is nowhere. */
static const fs_mount_t*
fs_resolve( const char* vpath, char* real_out )
{
    const fs_mount_t* best      = NULL;
    int               best_prio = 0;
    char              best_real[ FS_PATH_MAX ];

    for ( u32 i = 0; i < s_mount_count; ++i )
    {
        fs_mount_t* m = &s_mounts[ i ];
        if ( !m->used || !fs_prefix_match( vpath, m->vprefix ) )
            continue;

        /* Does this mount actually hold the file?  cand becomes the entry's "real": a real OS
           path for a DIR mount, or the in-archive name for a ZIP mount. */
        char cand[ FS_PATH_MAX ];
        if ( m->kind == FS_MOUNT_ZIP )
        {
            fs_zip_rel( m, vpath, cand );
            if ( fs_zip_locate( m, cand ) < 0 )
                continue;
        }
        else
        {
            if ( !fs_build_real( m, vpath, cand ) || !sys_file_exists( cand ) )
                continue;
        }

        if ( !best || m->priority > best_prio )
        {
            best      = m;
            best_prio = m->priority;
            snprintf( best_real, FS_PATH_MAX, "%s", cand );
        }
    }

    if ( best )
        snprintf( real_out, FS_PATH_MAX, "%s", best_real );
    return best;
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

void
fs_system_init( void )
{
    memset( s_mounts, 0, sizeof( s_mounts ) );
    s_mount_count   = 0;
    s_catalog       = ( fs_entry_t* )calloc( FS_MAX_FILES, sizeof( fs_entry_t ) );
    s_catalog_count = 0;
}

/* Release a ZIP mount's reader + in-memory archive (no-op for DIR mounts). */
static void
fs_mount_free_zip( fs_mount_t* m )
{
    if ( m->kind != FS_MOUNT_ZIP )
        return;
    if ( m->zip )
    {
        mz_zip_reader_end( m->zip );
        free( m->zip );
        m->zip = NULL;
    }
    free( m->zip_bytes );
    m->zip_bytes = NULL;
}

void
fs_system_exit( void )
{
    for ( u32 i = 0; i < s_mount_count; ++i )
        fs_mount_free_zip( &s_mounts[ i ] );

    free( s_catalog );
    s_catalog       = NULL;
    s_catalog_count = 0;
    s_mount_count   = 0;
}

/*==============================================================================================
    Mounts
==============================================================================================*/

/* Read a .zip whole into memory (sys owns disk I/O) and open a miniz reader over it.  Fills the
   ZIP fields of `m` and returns true, or frees whatever it took and returns false. */
static bool
fs_mount_zip_open( fs_mount_t* m, const char* zip_path )
{
    sys_file_data_t fd = sys_file_read_entire( zip_path );
    if ( !fd.ok )
    {
        return false;
    }

    mz_zip_archive* za = ( mz_zip_archive* )calloc( 1, sizeof( mz_zip_archive ) );
    if ( !za || !mz_zip_reader_init_mem( za, fd.data, fd.size, 0 ) )
    {
        free( za );
        free( fd.data );
        return false;
    }

    m->kind      = FS_MOUNT_ZIP;
    m->zip       = za;
    m->zip_bytes = fd.data;               // must outlive the reader (init_mem does not copy)
    m->zip_mtime = sys_file_time( zip_path );
    return true;
}

bool
fs_mount( const char* vprefix, const char* real_path, int priority )
{
    if ( s_mount_count >= FS_MAX_MOUNTS )
        return false;

    fs_mount_t* m = &s_mounts[ s_mount_count ];
    memset( m, 0, sizeof( *m ) );
    fs_norm_dir( m->vprefix, vprefix ? vprefix : "" );
    m->priority = priority;

    /* A real_path ending in ".zip" is a bundle mount; anything else is a directory. */
    if ( real_path && fs_has_zip_ext( real_path ) )
    {
        fs_norm_vpath( m->real, real_path );    // store the archive path verbatim (no trailing '/')
        if ( !fs_mount_zip_open( m, real_path ) )
            return false;                       // slot not consumed; bad/unreadable archive
    }
    else
    {
        m->kind = FS_MOUNT_DIR;
        fs_norm_dir( m->real, real_path ? real_path : "" );
    }

    m->used = true;
    ++s_mount_count;
    return true;
}

void
fs_unmount( const char* vprefix )
{
    char pfx[ FS_PATH_MAX ];
    fs_norm_dir( pfx, vprefix ? vprefix : "" );

    for ( u32 i = 0; i < s_mount_count; ++i )
    {
        if ( s_mounts[ i ].used && fs_ci_eq( s_mounts[ i ].vprefix, pfx ) )
        {
            fs_mount_free_zip( &s_mounts[ i ] );    // close the archive if this was a bundle
            s_mounts[ i ].used = false;
        }
    }

    /* Drop the cache; it rebuilds lazily and this avoids stale real-path entries. */
    if ( s_catalog )
        memset( s_catalog, 0, ( size_t )FS_MAX_FILES * sizeof( fs_entry_t ) );
    s_catalog_count = 0;
}

/*==============================================================================================
    Reads / queries
==============================================================================================*/

fs_blob_t
fs_read( const char* vpath_in )
{
    fs_blob_t blob = { NULL, 0, false };

    char vpath[ FS_PATH_MAX ];
    fs_norm_vpath( vpath, vpath_in );

    char              real[ FS_PATH_MAX ];
    fs_entry_t*       e = fs_catalog_find( vpath );
    const fs_mount_t* m = NULL;

    if ( e )
    {
        snprintf( real, FS_PATH_MAX, "%s", e->real );
        m = &s_mounts[ e->mount ];    // catalog entry remembers which mount (and thus kind) won
    }
    else if ( ( m = fs_resolve( vpath, real ) ) == NULL )
    {
        return blob;    // not found in any mount
    }

    /* `real` is an OS path for a DIR mount, or the in-archive entry name for a ZIP mount. */
    void* data  = NULL;
    u32   size  = 0;
    u64   mtime = 0;
    if ( m->kind == FS_MOUNT_ZIP )
    {
        data  = fs_zip_read( m, real, &size );
        mtime = m->zip_mtime;
    }
    else
    {
        sys_file_data_t fd = sys_file_read_entire( real );
        if ( fd.ok )
        {
            data  = fd.data;
            size  = fd.size;
            mtime = sys_file_time( real );
        }
    }
    if ( !data )
        return blob;

    blob.data = data;
    blob.size = size;
    blob.ok   = true;

    if ( !e )
        fs_catalog_insert( vpath, real, ( u16 )( m - s_mounts ), size, mtime );
    return blob;
}

void
fs_free( fs_blob_t* blob )
{
    if ( !blob )
        return;
    free( blob->data );    // fs_read buffers come from sys_file_read_entire (malloc)
    blob->data = NULL;
    blob->size = 0;
    blob->ok   = false;
}

bool
fs_exists( const char* vpath_in )
{
    char vpath[ FS_PATH_MAX ];
    fs_norm_vpath( vpath, vpath_in );

    if ( fs_catalog_find( vpath ) )
        return true;

    char              real[ FS_PATH_MAX ];
    const fs_mount_t* m = fs_resolve( vpath, real );
    if ( !m )
        return false;

    u32 size = 0;
    u64 mtime = 0;
    fs_entry_meta( m, real, &size, &mtime );
    fs_catalog_insert( vpath, real, ( u16 )( m - s_mounts ), size, mtime );
    return true;
}

bool
fs_stat( const char* vpath_in, fs_stat_t* out )
{
    if ( out )
    {
        out->size  = 0;
        out->mtime = 0;
        out->ok    = false;
    }

    char vpath[ FS_PATH_MAX ];
    fs_norm_vpath( vpath, vpath_in );

    /* Catalog hit.  For a DIR mount size/mtime are VOLATILE (the OS owns the file), so re-stat
       live -- this is what makes hot-reload observable (a rewritten loose file reports a new
       mtime); if it vanished, report a miss and let the entry go stale.  A ZIP entry is
       immutable in place, and its cached `real` is an in-archive name (not an OS path), so
       return the cached values as-is -- a zip-backed asset never spuriously hot-reloads. */
    fs_entry_t* e = fs_catalog_find( vpath );
    if ( e )
    {
        const fs_mount_t* em = &s_mounts[ e->mount ];
        if ( em->kind == FS_MOUNT_DIR )
        {
            u64 mtime = sys_file_time( e->real );
            if ( mtime == 0 )
                return false;    // gone from disk; caller keeps its last-good resource
            e->mtime = mtime;
            e->size  = sys_file_size( e->real );
        }
        if ( out )
        {
            out->size  = e->size;
            out->mtime = e->mtime;
            out->ok    = true;
        }
        return true;
    }

    char              real[ FS_PATH_MAX ];
    const fs_mount_t* m = fs_resolve( vpath, real );
    if ( !m )
        return false;

    u32 size  = 0;
    u64 mtime = 0;
    fs_entry_meta( m, real, &size, &mtime );
    fs_catalog_insert( vpath, real, ( u16 )( m - s_mounts ), size, mtime );

    if ( out )
    {
        out->size  = size;
        out->mtime = mtime;
        out->ok    = true;
    }
    return true;
}

/*==============================================================================================
    Glob  --  "vdir/pattern" across DIR mounts (flat within the named vdir).
==============================================================================================*/

typedef struct fs_glob_ctx_s
{
    const char* vdir;      // virtual directory of the matches (no trailing slash), or ""
    fs_glob_fn  cb;
    void*       userdata;
    int         count;
    bool        stop;

} fs_glob_ctx_t;

static bool
fs_glob_trampoline( const char* filename, const char* full_path, void* userdata )
{
    UNUSED( full_path );
    fs_glob_ctx_t* c = ( fs_glob_ctx_t* )userdata;

    char vpath[ FS_PATH_MAX ];
    if ( c->vdir[ 0 ] )
        snprintf( vpath, FS_PATH_MAX, "%s/%s", c->vdir, filename );
    else
        snprintf( vpath, FS_PATH_MAX, "%s", filename );

    ++c->count;
    if ( !c->cb( vpath, c->userdata ) )
    {
        c->stop = true;
        return false;
    }
    return true;
}

int
fs_glob( const char* vpat, fs_glob_fn cb, void* userdata )
{
    if ( !cb )
        return 0;

    char norm[ FS_PATH_MAX ];
    fs_norm_vpath( norm, vpat );

    /* Split into a virtual directory and a filename pattern at the last '/'. */
    char        vdir[ FS_PATH_MAX ];
    const char* pat   = norm;
    char*       slash = strrchr( norm, '/' );
    if ( slash )
    {
        size_t dl = ( size_t )( slash - norm );
        if ( dl >= FS_PATH_MAX )
            dl = FS_PATH_MAX - 1;
        memcpy( vdir, norm, dl );
        vdir[ dl ] = '\0';
        pat        = slash + 1;
    }
    else
    {
        vdir[ 0 ] = '\0';
    }

    /* vdir with a trailing slash, for prefix-matching against mount prefixes. */
    char vdir_slash[ FS_PATH_MAX ];
    fs_norm_dir( vdir_slash, vdir );

    fs_glob_ctx_t ctx = { vdir, cb, userdata, 0, false };

    for ( u32 i = 0; i < s_mount_count && !ctx.stop; ++i )
    {
        fs_mount_t* m = &s_mounts[ i ];
        if ( !m->used || m->kind != FS_MOUNT_DIR || !fs_prefix_match( vdir_slash, m->vprefix ) )
            continue;    // ZIP enumeration is not wired into glob this phase (reads still work)

        /* Real directory = mount real + (vdir beyond the mount prefix). */
        const char* rel = vdir_slash + strlen( m->vprefix );
        char        real_dir[ FS_PATH_MAX ];
        snprintf( real_dir, FS_PATH_MAX, "%s%s", m->real, rel );

        sys_file_glob( real_dir, pat, fs_glob_trampoline, &ctx );
    }

    return ctx.count;
}

/*==============================================================================================*/

u32
fs_file_count( void )
{
    return s_catalog_count;
}

/*============================================================================================*/
