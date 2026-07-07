#ifndef CORE_FS_H
#define CORE_FS_H
/*==============================================================================================

    engine/core/fs/fs.h

    Virtual filesystem -- the engine's single point for reading bytes by a virtual path.

    A vpath ("assets/hero.png") is resolved through an ordered MOUNT table onto a real
    backing.  Phase 1 supports DIR mounts (a real directory); a ZIP mount kind (Phase 5)
    plugs into the same read path.  Higher-priority mounts win on a path collision, so a
    loose file on disk overrides one shipped inside a bundle (the standard dev workflow).

    fs knows BYTES, never "assets".  It does not decode, does not hold GPU resources, and
    does not know what an image or a mesh is -- that lives one layer up in the asset service.

    CATALOG: a hashed vpath -> file index, used for O(1) repeat lookups and to answer "what
    files exist".  It is filled LAZILY for every mount kind: the first successful resolve
    caches the winning (mount, real) pair, and fs_read/fs_exists/fs_stat serve repeat lookups
    from it.  If a cached backing vanishes (a deleted loose override), the entry is evicted
    and the path re-resolves, so reads fall back to a lower-priority mount.  A NEW loose file
    that shadows a cataloged path is picked up by fs_stat: when a DIR mount sits above the
    cached winner, the stat re-resolves (this rides the asset service's hot-reload poll; a
    pack-only mount set skips it entirely).  fs_read/fs_exists serve the cached winner until
    a stat has refreshed the entry.  Mounting/unmounting drops the whole cache.

    Single-threaded: mount/read are expected on the main thread (matches the rest of core).

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Limits
==============================================================================================*/

#define FS_PATH_MAX     256     // bytes per virtual or real path (incl. NUL)
#define FS_MAX_MOUNTS   16      // concurrent mounts
#define FS_MAX_FILES    4096    // catalog capacity (power of two; open-addressing)

/*==============================================================================================
    Types
==============================================================================================*/

/* A whole-file blob read from the vfs.  `data` carries a hidden trailing NUL (so text
   payloads work as a C string); `size` is the real byte count and EXCLUDES that NUL.
   On failure: ok=false, data=NULL, size=0.  Release with fs_free() (safe on a failed read). */
typedef struct fs_blob_s
{
    void* data;
    u32   size;
    bool  ok;

} fs_blob_t;

/* Lightweight file metadata. `mtime` is an opaque platform timestamp -- compare only. */
typedef struct fs_stat_s
{
    u32  size;
    u64  mtime;
    bool ok;

} fs_stat_t;

/* fs_glob callback: `vpath` is the virtual path of a matching file, valid for the call only.
   Return true to keep iterating, false to stop early. */
typedef bool ( *fs_glob_fn )( const char* vpath, void* userdata );

/*==============================================================================================
    Lifecycle  (core-internal; called by core_init / core_exit)
==============================================================================================*/

void fs_system_init( void );
void fs_system_exit( void );

/*==============================================================================================
    Mounts
==============================================================================================*/

/* Map a virtual prefix onto a real directory.  `vprefix` may be "" to match every vpath.
   `priority` breaks collisions -- the highest-priority mount that actually has the file
   wins.  Returns false if the mount table is full. */
bool fs_mount( const char* vprefix, const char* real_path, int priority );

/* Remove the mount whose (normalized) virtual prefix matches `vprefix`.  Drops the catalog
   cache (it rebuilds lazily on the next read). */
void fs_unmount( const char* vprefix );

/*==============================================================================================
    Reads / queries  (resolved through the mount table + catalog)
==============================================================================================*/

fs_blob_t fs_read( const char* vpath );                   // read entire file; see fs_blob_t
void      fs_free( fs_blob_t* blob );                     // release a blob from fs_read
bool      fs_exists( const char* vpath );                 // true if the vpath resolves to a file
bool      fs_stat( const char* vpath, fs_stat_t* out );   // size + mtime without reading bytes
int       fs_glob( const char* vpat, fs_glob_fn cb, void* userdata );  // "dir/pattern" match
                                                          // DIR mounts only; ZIP entries are
                                                          // not enumerated (reads still work)

/* Number of files currently in the catalog (introspection / tests). */
u32       fs_file_count( void );

/*============================================================================================*/
#endif    // CORE_FS_H
