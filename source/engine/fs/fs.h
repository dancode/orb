#ifndef FS_H
#define FS_H
/*==============================================================================================

    engine/fs/fs.h -- virtual filesystem types, enums, and constants.

    Pure types only -- no vtable, no function declarations.
    Include engine/fs/fs_api.h for the runtime vtable + fs() gateway (DLL modules).
    Include engine/fs/fs_host.h for direct function calls (host exes / sandboxes).

    fs is a leaf engine library (deps: sys + pack), modeled on engine/prof: it sits below core
    so core, mod, app, and everything above them can read bytes by a virtual path without
    reaching back up into a higher layer.

    A vpath ("assets/hero.png") is resolved through an ordered MOUNT table onto a real backing.
    DIR mounts name a real directory; ZIP mounts serve entries out of a .zip.  Higher-priority
    mounts win on a path collision, so a loose file on disk overrides one shipped inside a
    bundle (the standard dev workflow).

    fs knows BYTES, never "assets".  It does not decode, does not hold GPU resources, and does
    not know what an image or a mesh is -- that lives one layer up in the asset service.

    CATALOG: a hashed vpath -> file index, used for O(1) repeat lookups and to answer "what
    files exist".  It is filled LAZILY for every mount kind: the first successful resolve caches
    the winning (mount, real) pair, and read/exists/stat serve repeat lookups from it.  If a
    cached backing vanishes (a deleted loose override), the entry is evicted and the path
    re-resolves, so reads fall back to a lower-priority mount.  A NEW loose file that shadows a
    cataloged path is picked up by stat: when a DIR mount sits above the cached winner, the stat
    re-resolves (this rides the asset service's hot-reload poll; a pack-only mount set skips it
    entirely).  read/exists serve the cached winner until a stat has refreshed the entry.
    Mounting/unmounting drops the whole cache.

    Single-threaded: mount/read are expected on the main thread (matches the rest of the engine).

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
   On failure: ok=false, data=NULL, size=0.  Release with fs()->free (safe on a failed read). */
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

/* fs glob callback: `vpath` is the virtual path of a matching file, valid for the call only.
   Return true to keep iterating, false to stop early. */
typedef bool ( *fs_glob_fn )( const char* vpath, void* userdata );

/*============================================================================================*/
#endif    // FS_H
