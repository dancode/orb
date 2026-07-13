/*==============================================================================================

    engine/fs/fs.c -- virtual filesystem implementation (DIR mounts + lazy catalog).

    Resolution: normalize the vpath, then among mounts whose vprefix matches, pick the
    highest-priority mount that actually has the file (loose-over-bundle).  The first hit is
    cached in the catalog so repeat lookups are O(1) and never re-scan the mount table.

==============================================================================================*/

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#include "orb.h"
#include "engine/fs/fs_host.h"        /* types + direct decls (fs is a sys-only leaf) */
#include "engine/fs/fs_zip.h"         /* miniz config -- must precede vendor/miniz.h */
#include "vendor/miniz.h"             /* mz_zip_* reader (impl in fs_zip_miniz.c) */
#include "engine/sys/sys_host.h"      /* sys_file_read_entire / _exists / _size / _time / _glob */

/* Catalog hash: case-insensitive FNV-1a over the vpath.  fs sits below core, so it cannot call
   core's sid_hash_len -- it keeps its own copy of the identical algorithm (a private catalog
   hash never needs to agree with a SID anyway).  Matches sid.h so behaviour is unchanged. */
static u32
fs_hash( const char* str, size_t len )
{
    u32 h = 2166136261u;
    for ( size_t i = 0; i < len; i++ )
    {
        unsigned char c = ( unsigned char )str[ i ];
        if ( c >= 'A' && c <= 'Z' ) c = ( unsigned char )( c + 32 );
        h = ( h ^ c ) * 16777619u;
    }
    return h;
}

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
    u32  hash;                     // fs_hash of vpath
    u16  mount;                    // owning mount index
    u16  used;                     // 0 empty, 1 used, 2 tombstone (evicted; keeps probe chains)
    u32  size;
    u64  mtime;
    char vpath[ FS_PATH_MAX ];     // full virtual path (catalog key)
    char real[ FS_PATH_MAX ];      // resolved real OS path (avoids re-resolve on read)

} fs_entry_t;

static fs_mount_t  s_mounts[ FS_MAX_MOUNTS ];
static u32         s_mount_count;    // LIVE mounts; slots may be sparse (iterate FS_MAX_MOUNTS)
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

/* Read the whole file behind a resolved (mount, real) pair, dispatched by mount kind.  Returns
   a malloc'd buffer (hidden trailing NUL per the fs_blob_t contract) or NULL if the backing is
   gone/unreadable; fills size + mtime on success. */
static void*
fs_read_backing( const fs_mount_t* m, const char* real, u32* out_size, u64* out_mtime )
{
    if ( m->kind == FS_MOUNT_ZIP )
    {
        void* data = fs_zip_read( m, real, out_size );
        if ( data )
            *out_mtime = m->zip_mtime;
        return data;
    }

    sys_file_data_t fd = sys_file_read_entire( real );
    if ( !fd.ok )
        return NULL;
    *out_size  = fd.size;
    *out_mtime = sys_file_time( real );
    return fd.data;
}

/*==============================================================================================
    Catalog
==============================================================================================*/

static fs_entry_t*
fs_catalog_find( const char* vpath )
{
    if ( !s_catalog )
        return NULL;

    u32 hash = fs_hash( vpath, strlen( vpath ) );
    u32 mask = FS_MAX_FILES - 1;
    u32 i    = hash & mask;

    for ( u32 n = 0; n < FS_MAX_FILES; ++n )
    {
        fs_entry_t* e = &s_catalog[ i ];
        if ( e->used == 0 )
            return NULL;    // empty slot ends the probe chain (tombstones keep it alive)
        if ( e->used == 1 && e->hash == hash && fs_ci_eq( e->vpath, vpath ) )
            return e;
        i = ( i + 1 ) & mask;
    }
    return NULL;
}

/* Evict a stale entry (its backing vanished).  Tombstoned rather than emptied so probe chains
   through this slot stay intact; fs_catalog_insert reuses tombstones. */
static void
fs_catalog_evict( fs_entry_t* e )
{
    e->used = 2;
    if ( s_catalog_count > 0 )
        --s_catalog_count;
}

static fs_entry_t*
fs_catalog_insert( const char* vpath, const char* real, u16 mount, u32 size, u64 mtime )
{
    if ( !s_catalog )
        return NULL;
    if ( s_catalog_count >= ( FS_MAX_FILES * 3 ) / 4 )
        return NULL;    // keep the load factor sane; reads still work uncached

    u32 hash = fs_hash( vpath, strlen( vpath ) );
    u32 mask = FS_MAX_FILES - 1;
    u32 i    = hash & mask;

    for ( u32 n = 0; n < FS_MAX_FILES; ++n )
    {
        fs_entry_t* e = &s_catalog[ i ];
        if ( e->used != 1 )    // reuse empty or tombstone (insert only runs after a failed find)
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

    for ( u32 i = 0; i < FS_MAX_MOUNTS; ++i )
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

/* True if a DIR mount other than `em` sits ABOVE the cached winner for `vpath` -- meaning a
   loose file may have APPEARED there since the entry was cataloged (editor workflow: drop an
   override next to a mounted pack).  Only DIR mounts can gain files in place, so this is the
   gate for re-resolving a catalog hit; with no DIR mount above the winner (shipping: pack-only
   mounts) it returns false and cached stats stay as cheap as before. */
static bool
fs_shadow_possible( const char* vpath, const fs_mount_t* em )
{
    for ( u32 i = 0; i < FS_MAX_MOUNTS; ++i )
    {
        const fs_mount_t* m = &s_mounts[ i ];
        if ( !m->used || m == em || m->kind != FS_MOUNT_DIR )
            continue;
        if ( m->priority > em->priority && fs_prefix_match( vpath, m->vprefix ) )
            return true;
    }
    return false;
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
    for ( u32 i = 0; i < FS_MAX_MOUNTS; ++i )
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
    /* Reuse the first free slot -- unmount frees slots, so mount/unmount cycles never exhaust
       the table. */
    fs_mount_t* m = NULL;
    for ( u32 i = 0; i < FS_MAX_MOUNTS; ++i )
    {
        if ( !s_mounts[ i ].used )
        {
            m = &s_mounts[ i ];
            break;
        }
    }
    if ( !m )
        return false;

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

    /* A new mount can outrank cached winners; drop the cache like unmount does (rebuilds lazily). */
    if ( s_catalog )
        memset( s_catalog, 0, ( size_t )FS_MAX_FILES * sizeof( fs_entry_t ) );
    s_catalog_count = 0;
    return true;
}

void
fs_unmount( const char* vprefix )
{
    char pfx[ FS_PATH_MAX ];
    fs_norm_dir( pfx, vprefix ? vprefix : "" );

    for ( u32 i = 0; i < FS_MAX_MOUNTS; ++i )
    {
        if ( s_mounts[ i ].used && fs_ci_eq( s_mounts[ i ].vprefix, pfx ) )
        {
            fs_mount_free_zip( &s_mounts[ i ] );    // close the archive if this was a bundle
            s_mounts[ i ].used = false;
            --s_mount_count;
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

    /* `real` is an OS path for a DIR mount, or the in-archive entry name for a ZIP mount. */
    char              real[ FS_PATH_MAX ];
    fs_entry_t*       e = fs_catalog_find( vpath );
    const fs_mount_t* m = NULL;

    void* data  = NULL;
    u32   size  = 0;
    u64   mtime = 0;

    if ( e )
    {
        snprintf( real, FS_PATH_MAX, "%s", e->real );
        m    = &s_mounts[ e->mount ];    // catalog entry remembers which mount (and thus kind) won
        data = fs_read_backing( m, real, &size, &mtime );
        if ( !data )
        {
            /* Backing vanished (e.g. a deleted loose override): evict the stale entry and
               re-resolve so a lower-priority mount can serve the path again. */
            fs_catalog_evict( e );
            e = NULL;
        }
    }

    if ( !data )
    {
        m = fs_resolve( vpath, real );
        if ( !m )
            return blob;    // not found in any mount
        data = fs_read_backing( m, real, &size, &mtime );
        if ( !data )
            return blob;
    }

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

    fs_entry_t* e = fs_catalog_find( vpath );
    if ( e )
    {
        /* A DIR backing is volatile (the OS owns the file) -- verify it is still there; if it
           vanished, evict and fall through to a fresh resolve (a lower mount may still have it).
           A ZIP entry is immutable in place, so the cached hit stands. */
        const fs_mount_t* em = &s_mounts[ e->mount ];
        if ( em->kind != FS_MOUNT_DIR || sys_file_exists( e->real ) )
            return true;
        fs_catalog_evict( e );
    }

    char              real[ FS_PATH_MAX ];
    const fs_mount_t* m = fs_resolve( vpath, real );
    if ( !m )
        return false;

    u32 size = 0;
    u64 mtime = 0;
    if ( !fs_entry_meta( m, real, &size, &mtime ) )
        return false;    // vanished in the resolve->stat window; do not cache zeros
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

    /* Catalog hit.  fs_stat is the hot-reload probe (asset()->refresh polls it per live asset),
       so it must answer "did the world change for this path" in full:
         - a higher-priority DIR mount may have GAINED a shadowing file -> evict + re-resolve;
           the fresh winner's differing mtime is what the refresh poll keys on.
         - a DIR winner's size/mtime are VOLATILE (the OS owns the file) -> re-stat live; if it
           vanished, evict + re-resolve so a shadowed lower mount answers again.
         - a ZIP entry is immutable in place and nothing sits above it -> cached values as-is,
           so a zip-backed asset never spuriously hot-reloads and shipping stats stay cheap. */
    fs_entry_t* e = fs_catalog_find( vpath );
    if ( e )
    {
        const fs_mount_t* em = &s_mounts[ e->mount ];
        if ( fs_shadow_possible( vpath, em ) )
        {
            fs_catalog_evict( e );
            e = NULL;    // a new loose override may exist; re-resolve below
        }
        else if ( em->kind == FS_MOUNT_DIR )
        {
            u64 mtime = sys_file_time( e->real );
            if ( mtime == 0 )
            {
                fs_catalog_evict( e );
                e = NULL;    // gone from disk; re-resolve below
            }
            else
            {
                e->mtime = mtime;
                e->size  = sys_file_size( e->real );
            }
        }
        if ( e )
        {
            if ( out )
            {
                out->size  = e->size;
                out->mtime = e->mtime;
                out->ok    = true;
            }
            return true;
        }
    }

    char              real[ FS_PATH_MAX ];
    const fs_mount_t* m = fs_resolve( vpath, real );
    if ( !m )
        return false;

    u32 size  = 0;
    u64 mtime = 0;
    if ( !fs_entry_meta( m, real, &size, &mtime ) )
        return false;    // vanished in the resolve->stat window; do not cache zeros
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

    for ( u32 i = 0; i < FS_MAX_MOUNTS && !ctx.stop; ++i )
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

/*==============================================================================================
    API struct + module descriptor -- folded into this unity TU so g_fs_api_struct sees every
    implementation function above.
==============================================================================================*/

#include "engine/fs/fs_api.c"

/*============================================================================================*/
