#ifndef ORB_FONT_H
#define ORB_FONT_H
/*==============================================================================================

    tools/font_tool/orb_font.h -- .orb_font binary file format.

    File layout (all values little-endian):
        orb_font_header_t   header
        orb_font_glyph_t    glyphs[ header.glyph_count ]
        uint8_t             pixels[ header.atlas_w * header.atlas_h ]  (R8 grayscale)

==============================================================================================*/

#include <stdint.h>

/* 'OFNT' -- bytes O,F,N,T in little-endian memory order */
#define ORB_FONT_MAGIC    0x544E464Fu
#define ORB_FONT_VERSION  1u

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t atlas_w;
    uint32_t atlas_h;
    uint32_t font_size;     /* rendered glyph height in pixels          */
    int32_t  ascent;        /* pixels above baseline (positive)         */
    int32_t  descent;       /* pixels below baseline (negative)         */
    int32_t  line_gap;      /* extra spacing beyond ascent+descent      */
    uint32_t glyph_count;

    /* immediately followed by glyph_count * orb_font_glyph_t, then pixel data */

} orb_font_header_t;

typedef struct
{
    uint32_t codepoint;     /* Unicode codepoint (32..126 for our built-in fonts) */    
    uint16_t atlas_x;       /* pixel origin in atlas                     */
    uint16_t atlas_y;       /* pixel origin in atlas                     */
    uint16_t w;             /* bitmap width in pixels (0 for whitespace) */
    uint16_t h;             /* bitmap height in pixels                   */
    int16_t  bearing_x;     /* cursor-to-left-edge offset in pixels      */
    int16_t  bearing_y;     /* baseline-to-top-edge offset (positive up) */
    uint16_t advance;       /* horizontal cursor advance in pixels       */
    uint16_t _pad;

} orb_font_glyph_t;

#endif  /* ORB_FONT_H */
