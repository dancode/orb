#ifndef ORB_FONT_H
#define ORB_FONT_H
/*==============================================================================================

    tools/font_tool/orb_font.h -- .orb_font binary file format.

    File layout (all values little-endian):
        orb_font_header_t   header                          (opens with res_ref_head_t)
        uint8_t             refs[ header.ref_size ]         (engine/res/res_ref.h)
        orb_font_glyph_t    glyphs[ header.glyph_count ]
        uint8_t             pixels[ header.atlas_w * header.atlas_h ]  (R8: coverage, or a
                                                                        distance field when
                                                                        header.sdf_range > 0)

==============================================================================================*/

#include <stdint.h>

#include "engine/res/res_ref.h"   /* the reference section between header and glyphs */

/* 'OFNT' -- bytes O,F,N,T in little-endian memory order */
#define ORB_FONT_MAGIC    0x544E464Fu

/* Format versions.  The loader requires the current one outright; nothing older parses.
     7  The header opens with the res_ref_head_t fields (engine/res/res_ref.h) and the
        reference section is a padded string table sized by `ref_size`, located by
        `ref_offset`.  A font names nothing, so every bake writes both as 0.
     6  Header gained `ref_count` and the file gained a reference section of u32 ids.
     5  orb_font_glyph_t shrank: w, h, advance became uint8_t and bearing_x, bearing_y became
        int8_t (ORB_FONT_GLYPH_DIM_MAX / ORB_FONT_GLYPH_BEARING_MIN/MAX), and the trailing _pad
        was dropped.  The baker (dev_font_bake_write) rejects any glyph that would not fit these
        ranges, so a file never carries a truncated value.
     4  Header gained `sdf_range`, so a font can be a DISTANCE FIELD instead of coverage.
     3  Glyphs are packed full-height; the atlas is pure glyph coverage, no reserved band.  The gui
        runtime draws its white texel + dash-pattern rows from a shared resource atlas
        (gui_res_atlas.c), so a font no longer carries drawing assists of its own.
     2  Left the bottom 5 rows blank for gui to paint assists into at load. */
#define ORB_FONT_VERSION  7u

/* Per-glyph metric ceiling backing the u8/i8 fields below: a bitmap dimension or the horizontal
   advance never exceeds this many pixels, and a bearing never leaves this signed range.  Text
   that needs to scale past it bakes as an SDF instead -- one moderate-resolution bitmap scales
   freely at draw time, so a raw coverage glyph never legitimately needs to be this large.  The
   baker (dev_font_bake_write) rejects an oversized glyph outright; nothing narrows it. */
#define ORB_FONT_GLYPH_DIM_MAX      255
#define ORB_FONT_GLYPH_BEARING_MIN (-128)
#define ORB_FONT_GLYPH_BEARING_MAX  127

/* DEFAULT baked codepoint range -- the ASCII printable span U+0020 (space) .. U+007E (tilde).
   This is the contract shared by both bakers (dev_font, font_tool) and the runtime loader's dense
   ASCII table, so the glyph count and the codepoint->slot mapping have exactly one definition.
   Glyph records are sparse by codepoint on disk, so a file may carry MORE than this span
   (font_tool -range); anything up to ORB_FONT_MAX_GLYPHS records is a valid file. */
#define ORB_FONT_CP_FIRST  32u
#define ORB_FONT_CP_LAST   126u
#define ORB_FONT_CP_COUNT  ( ORB_FONT_CP_LAST - ORB_FONT_CP_FIRST + 1u )   /* 95 */

/* Cap on glyph records in one file.  Bounds every fixed buffer on both sides of the format --
   baker scratch, pack rects, and the runtime loader's sanity check -- so "how many glyphs can a
   font carry" has exactly one answer.  Sized for full European coverage (Latin + Extended-A +
   Greek + Cyrillic is ~700) with headroom; CJK-scale sets are a different architecture (demand
   paging), not a bigger constant. */
#define ORB_FONT_MAX_GLYPHS  2048u

/* Max PAGE width by destination.  The gui runtime uploads a baked page into a shared atlas as
   ONE tenant -- it is never re-packed per glyph -- so the page's shape is its permanent
   footprint there, and the atlas has to grow to this width plus its gutter before the page can
   be placed at all.  Coverage packs with a 1px pad, distance fields with 2px (the extrude ring);
   both caps take 2 off so the number stays right even if coverage gains a ring.  The values
   bound the BAKER: they keep the rung the runtime must double up to modest -- 512 wide for
   coverage, 1024 for SDF -- well under GUI_RES_ATLAS_DIM_CAP (gui_res_atlas.h), which is where
   a page is rejected outright.  The runtime atlases boot far smaller and grow on demand, so
   these are not their dimensions.  This header is the one contract the baker and the runtime
   already share, which is what makes that coupling explicit rather than implied. */
#define ORB_FONT_PAGE_MAX_W_COVERAGE  510u
#define ORB_FONT_PAGE_MAX_W_SDF       1022u

typedef struct
{
    /* The res_ref_head_t fields (engine/res/res_ref.h): every cooked format opens with these. */
    uint32_t magic;         /* ORB_FONT_MAGIC                                             */
    uint32_t version;       /* ORB_FONT_VERSION                                           */
    uint32_t ref_count;     /* names in the reference section; 0 -- a font names nothing  */
    uint32_t ref_size;      /* padded bytes of the reference section; 0 today             */
    uint32_t ref_offset;    /* where the section starts: sizeof( orb_font_header_t )      */

    uint32_t atlas_w;
    uint32_t atlas_h;
    uint32_t font_size;     /* rendered glyph height in pixels          */
    int32_t  ascent;        /* pixels above baseline (positive)         */
    int32_t  descent;       /* pixels below baseline (negative)         */
    int32_t  line_gap;      /* extra spacing beyond ascent+descent      */
    uint32_t glyph_count;

    /* 0  -- the pixels are COVERAGE: the byte is alpha, 0 = empty, 255 = solid.
       >0 -- the pixels are a SIGNED DISTANCE FIELD and this is its SPREAD in pixels.  The byte
             encoding is FreeType's (src/sdf): 128 is exactly ON the outline, >128 inside, <128
             outside, and 127 byte-steps span `sdf_range` pixels.  So a texel's signed distance in
             pixels is ( byte - 128 ) / 127 * sdf_range.
             The GUI does not need that scale to render -- it recovers the edge with a screen-space
             derivative, which is what makes an SDF font scale and rotate for free -- but the number
             is what makes the file self-describing, and `font_tool info` prints it. */
    uint32_t sdf_range;

    /* immediately followed by the reference section, glyph_count * orb_font_glyph_t, then pixel
       data */

} orb_font_header_t;

RES_REF_HEAD_ASSERT( orb_font_header_t );

typedef struct
{
    uint32_t codepoint;     /* Unicode codepoint (32..126 for our built-in fonts) */
    uint16_t atlas_x;       /* pixel origin in atlas                     */
    uint16_t atlas_y;       /* pixel origin in atlas                     */
    uint8_t  w;             /* bitmap width in pixels (0 for whitespace); ORB_FONT_GLYPH_DIM_MAX ceiling */
    uint8_t  h;             /* bitmap height in pixels; ORB_FONT_GLYPH_DIM_MAX ceiling */
    int8_t   bearing_x;     /* cursor-to-left-edge offset in pixels; ORB_FONT_GLYPH_BEARING_MIN/MAX */
    int8_t   bearing_y;     /* baseline-to-top-edge offset (positive up); ORB_FONT_GLYPH_BEARING_MIN/MAX */
    uint8_t  advance;       /* horizontal cursor advance in pixels; ORB_FONT_GLYPH_DIM_MAX ceiling */

} orb_font_glyph_t;

#endif  /* ORB_FONT_H */

/*============================================================================================*/