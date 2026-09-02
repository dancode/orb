#ifndef ASSET_TEX_H
#define ASSET_TEX_H
/*==============================================================================================

    runtime_service/asset/loaders/asset_tex.h -- cooked texture format (.tex).

    The .tex file is the CONTRACT between the offline cooker and the runtime loader:

        writer  -- asset_tool's image converter decodes a source PNG/JPG/... to RGBA8 and
                   writes this header followed by the tightly packed pixel payload.
        reader  -- the asset service's image loader (loaders/asset_image.c) memory-maps the
                   payload straight onto a bindless RHI texture with ZERO decode.

    Layout is a fixed 44-byte header (all fields little-endian u32; the tool and the engine run
    on the same architecture), the reference section, then `data_size` bytes of pixel data:

        [ asset_tex_header_t ][ refs: ref_size bytes ][ mip 0 pixels ... ]

    The header opens with the res_ref_head_t fields (engine/res/res_ref.h), which size and
    locate the reference section -- the resources the content names.  An image names none,
    so every cooked .tex has ref_count 0 and ref_size 0 and the loader steps over an empty
    section.  Only mip 0 / RGBA8 exists today (mip_levels == 1).  The header carries mip_levels
    and a reserved flags word so a full mip chain or a block-compressed format can be added
    later without breaking the magic/version handshake.

    This header is intentionally dependency-free (u32 from orb.h and the header-only res_ref.h)
    so asset_tool -- which links base + sys only, no engine runtime -- can include it alongside
    the runtime loader.

    Versions:
        3  header opens with res_ref_head_t; the reference section is a padded string table.
        2  ref_count + a reference section of u32 ids between header and pixels.
        1  header + pixels.

==============================================================================================*/

#include "orb.h"
#include "engine/res/res_ref.h"

/* 'O','T','E','X' in file order (little-endian store). Distinguishes a cooked .tex from a raw
   source image the loader might also be handed. */
#define ASSET_TEX_MAGIC \
    ( ( u32 )'O' | ( ( u32 )'T' << 8 ) | ( ( u32 )'E' << 16 ) | ( ( u32 )'X' << 24 ) )

#define ASSET_TEX_VERSION 3

/* Pixel formats. Kept minimal; maps 1:1 onto an RHI format at load time. */
enum
{
    ASSET_TEX_FORMAT_RGBA8 = 1,   // 8-bit unorm RGBA, 4 bytes/texel
};

typedef struct asset_tex_header_s
{
    u32 magic;        // ASSET_TEX_MAGIC
    u32 version;      // ASSET_TEX_VERSION
    u32 ref_count;    // names in the reference section (res_ref.h); 0 -- an image names nothing
    u32 ref_size;     // padded bytes of the reference section; 0 today
    u32 ref_offset;   // where the section starts: sizeof( asset_tex_header_t )

    u32 width;        // mip 0 width in texels
    u32 height;       // mip 0 height in texels
    u32 format;       // ASSET_TEX_FORMAT_*
    u32 mip_levels;   // 1 for now (no mip chain yet)
    u32 data_size;    // pixel payload bytes following the reference section
    u32 flags;        // reserved (0); future: sRGB, premultiplied, block-compressed, ...

} asset_tex_header_t;

RES_REF_HEAD_ASSERT( asset_tex_header_t );

/*============================================================================================*/
#endif    // ASSET_TEX_H
