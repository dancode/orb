#ifndef PACK_API_H
#define PACK_API_H
/*==============================================================================================

    engine/pack/pack_api.h -- pack module API struct and pack() gateway macro.
    pack is always statically linked into the host.

    The vtable carries the basic buffer tasks a module might need (compress/decompress a blob,
    checksum).  Archive reading/writing is host-side work -- fs and the tools call the direct
    functions in pack_host.h -- so it is deliberately absent here.

==============================================================================================*/

#include "engine/pack/pack.h"
#include "engine/mod/mod_import.h"

/*==============================================================================================
    API Struct

    Vtable names drop the pack_ prefix -- the gateway supplies the subject: pack()->inflate().
==============================================================================================*/

typedef struct pack_api_s
{
    /* Worst-case deflated size for `src_size` input bytes -- size the destination of a
       deflate() call with this. */
    u32 ( *bound )( u32 src_size );

    /* Deflate src into dst (zlib stream).  On call *dst_size is the destination capacity; on
       success it holds the written byte count.  `level` is PACK_LEVEL_* (or any 0..10).
       Returns false if dst is too small. */
    bool ( *deflate )( const void* src, u32 src_size, void* dst, u32* dst_size, int level );

    /* Inflate src into dst.  On call *dst_size is the destination capacity -- the caller is
       expected to know the decompressed size (store it beside the payload); on success it
       holds the written byte count.  Returns false on corrupt input or a short buffer. */
    bool ( *inflate )( const void* src, u32 src_size, void* dst, u32* dst_size );

    /* Incremental CRC-32 (zlib polynomial).  Pass 0 to start, the previous return to
       continue. */
    u32 ( *crc32 )( u32 crc, const void* data, u32 size );

} pack_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( PACK_STATIC )
    MOD_GATEWAY_STATIC( pack_api_t, pack )
    #define MOD_USE_PACK    /* static build */
    #define MOD_FETCH_PACK  true
#else
    MOD_GATEWAY_DYNAMIC( pack_api_t, pack )
    #define MOD_USE_PACK    MOD_DEFINE_API_PTR( pack_api_t, pack )
    #define MOD_FETCH_PACK  MOD_FETCH_API( pack_api_t, pack )
#endif

/*============================================================================================*/
#endif    // PACK_API_H
