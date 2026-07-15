#ifndef PACK_HOST_H
#define PACK_HOST_H
/*==============================================================================================

    engine/pack/pack_host.h -- host-only pack API: direct calls and the module descriptor.
    Includes pack_api.h.

    The direct surface is wider than the module vtable on purpose: modules get the basic
    buffer tasks through pack() (pack_api.h), while the real work -- ZIP archive reading and
    writing -- lives here for the clients that statically link pack: fs (zip mounts),
    asset_tool (bundle cook), and sandboxes building test archives in memory.

==============================================================================================*/

#include "engine/pack/pack_api.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

/*==============================================================================================
    Direct-call functions (host, engine libs, and tools)

    Buffer codec -- twins of the pack_api_t vtable; see pack_api.h for the contracts.
==============================================================================================*/

u32         pack_bound   ( u32 src_size );
bool        pack_deflate ( const void* src, u32 src_size, void* dst, u32* dst_size, int level );
bool        pack_inflate ( const void* src, u32 src_size, void* dst, u32* dst_size );
u32         pack_crc32   ( u32 crc, const void* data, u32 size );

/*==============================================================================================
    ZIP reading

    A reader BORROWS the archive bytes: `data` must stay alive and unmoved until
    pack_zip_close (matches the fs mount pattern -- the whole .zip is held in memory anyway).
==============================================================================================*/

/* Open a reader over an in-memory archive.  Returns NULL if the bytes are not a zip. */
pack_zip_t* pack_zip_open    ( const void* data, u32 size );
void        pack_zip_close   ( pack_zip_t* zip );

/* Number of entries in the archive. */
u32         pack_zip_count   ( pack_zip_t* zip );

/* Entry index for `name` (case-insensitive), or -1 if absent. */
int         pack_zip_find    ( pack_zip_t* zip, const char* name );

/* Name + uncompressed size of the entry at `index`.  Returns false on a bad index. */
bool        pack_zip_stat    ( pack_zip_t* zip, int index, pack_zip_stat_t* out );

/* Decompress the entry at `index` into dst (capacity dst_size; must hold the entry's
   uncompressed size -- see pack_zip_stat).  Returns false on a bad index, a short buffer,
   or corrupt data. */
bool        pack_zip_extract ( pack_zip_t* zip, int index, void* dst, u32 dst_size );

/*==============================================================================================
    ZIP writing

    The archive is built in a heap block -- pack never touches the disk; the caller writes
    the finished blob out through sys (or wherever), matching the engine's I/O ownership.
==============================================================================================*/

/* Start an in-memory archive.  Returns NULL on allocation failure. */
pack_zip_writer_t* pack_zip_writer_begin( void );

/* Add one entry from memory.  `level` is PACK_LEVEL_* (or any 0..10). */
bool        pack_zip_writer_add  ( pack_zip_writer_t* w, const char* name,
                                   const void* data, u32 size, int level );

/* Finalize: central directory is written and the finished archive is handed out as a
   malloc'd blob the caller owns (release with free()).  The writer is destroyed either way;
   on failure *out_data is NULL / *out_size is 0. */
bool        pack_zip_writer_end  ( pack_zip_writer_t* w, void** out_data, u32* out_size );

/* Destroy a writer without producing an archive (error-path teardown). */
void        pack_zip_writer_abort( pack_zip_writer_t* w );

/*==============================================================================================
    Module Descriptor

    Used by the host to register the pack module:
        mod_static_load( "pack", pack_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_static( pack );

    pack is stateless (pure transforms), so init/exit are trivial -- registration exists so
    modules can fetch pack_api_t through the standard gateway.
==============================================================================================*/

mod_desc_t* pack_get_mod_desc( void );

/*============================================================================================*/
#endif    // PACK_HOST_H
