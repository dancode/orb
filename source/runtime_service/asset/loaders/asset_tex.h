#ifndef ASSET_TEX_H
#define ASSET_TEX_H
/*==============================================================================================

    runtime_service/asset/loaders/asset_tex.h -- cooked texture format (.tex).

    The .tex file is the CONTRACT between the offline cooker and the runtime loader:

        writer  -- asset_tool's image converter decodes a source PNG/JPG/... to RGBA8 and
                   writes this header followed by the tightly packed pixel payload.
        reader  -- the asset service's image loader (loaders/asset_image.c) memory-maps the
                   payload straight onto a bindless RHI texture with ZERO decode.

    Layout is a fixed 32-byte header (all fields little-endian u32; the tool and the engine run
    on the same architecture) immediately followed by `data_size` bytes of pixel data:

        [ asset_tex_header_t ][ mip 0 pixels ... ]

    Only mip 0 / RGBA8 exists today (mip_levels == 1).  The header carries mip_levels and a
    reserved flags word so a full mip chain or a block-compressed format can be added later
    without breaking the magic/version handshake.

    This header is intentionally dependency-free (just u32 from orb.h) so asset_tool -- which
    links base + sys only, no engine runtime -- can include it alongside the runtime loader.

==============================================================================================*/

#include "orb.h"

/* 'O','T','E','X' in file order (little-endian store). Distinguishes a cooked .tex from a raw
   source image the loader might also be handed. */
#define ASSET_TEX_MAGIC \
    ( ( u32 )'O' | ( ( u32 )'T' << 8 ) | ( ( u32 )'E' << 16 ) | ( ( u32 )'X' << 24 ) )

#define ASSET_TEX_VERSION 1

/* Pixel formats. Kept minimal; maps 1:1 onto an RHI format at load time. */
enum
{
    ASSET_TEX_FORMAT_RGBA8 = 1,   // 8-bit unorm RGBA, 4 bytes/texel
};

typedef struct asset_tex_header_s
{
    u32 magic;        // ASSET_TEX_MAGIC
    u32 version;      // ASSET_TEX_VERSION
    u32 width;        // mip 0 width in texels
    u32 height;       // mip 0 height in texels
    u32 format;       // ASSET_TEX_FORMAT_*
    u32 mip_levels;   // 1 for now (no mip chain yet)
    u32 data_size;    // payload bytes following this header
    u32 flags;        // reserved (0); future: sRGB, premultiplied, block-compressed, ...

} asset_tex_header_t;

/*============================================================================================*/
#endif    // ASSET_TEX_H
