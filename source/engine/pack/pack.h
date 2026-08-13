#ifndef PACK_H
#define PACK_H
/*==============================================================================================

    engine/pack/pack.h -- compression types, enums, and constants.

    Pure types only -- no vtable, no function declarations.
    Include engine/pack/pack_api.h for the runtime vtable + pack() gateway (DLL modules).
    Include engine/pack/pack_host.h for direct function calls (host exes / engine libs / tools).

    pack is the engine's data-encoding substrate: deflate/inflate buffers, crc32, and ZIP
    archive reading/writing, backed by the single engine-wide copy of the vendored miniz
    amalgamation (pack_miniz.c).  It is a leaf library (no deps) of pure memory-to-memory
    transforms -- no OS calls, no state to initialize -- so engine libraries (fs mounts zips
    through it) and standalone tools (asset_tool writes bundles through it) link the same code.

    The exported surface is deliberately lean: buffer codec + the archive operations the
    engine actually performs.  miniz itself stays quarantined inside the pack library -- no
    other target includes vendor/miniz.h or compiles its own copy, so the amalgamation can
    never collide at link time; fs and asset_tool each vendoring their own copy is exactly
    the setup this rules out.

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Constants
==============================================================================================*/

/* Compression levels (deflate effort; miniz range).  NONE stores raw. */
#define PACK_LEVEL_NONE     0
#define PACK_LEVEL_FAST     1
#define PACK_LEVEL_DEFAULT  6
#define PACK_LEVEL_BEST     10

#define PACK_ZIP_NAME_MAX   260     // bytes per archive entry name (incl. NUL)

/*==============================================================================================
    Types
==============================================================================================*/

/* Opaque archive handles, heap-allocated by pack (the underlying miniz state never crosses
   the API).  A reader borrows the caller's archive bytes; a writer builds into its own heap. */
typedef struct pack_zip_s        pack_zip_t;
typedef struct pack_zip_writer_s pack_zip_writer_t;

/* Metadata for one archive entry (pack_zip_stat).  `size` is the UNCOMPRESSED byte count --
   what pack_zip_extract needs the destination to hold. */
typedef struct pack_zip_stat_s
{
    char name[ PACK_ZIP_NAME_MAX ];
    u32  size;

} pack_zip_stat_t;

/*============================================================================================*/
#endif    // PACK_H
