/*==============================================================================================

    tools/font_tool/font_tool.c -- Offline font atlas baker.

    Rasterizes a TTF/OTF font at a given pixel size using FreeType, packs the
    glyph bitmaps into a texture atlas with stb_rect_pack, and writes an
    .orb_font binary file that the engine can load directly at runtime.

    Usage:
        font_tool.exe <input.ttf> <size_px> <output.orb_font>

    Bakes ASCII printable range U+0020..U+007E (95 glyphs).
    Output atlas is always 512x512, R8 grayscale.
    The output path must include any directory; no directories are created.

    Link deps: freetype.lib (import lib for freetype.dll)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include "tools/font_tool/stb_rect_pack.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include "tools/font_tool/orb_font.h"

/*==============================================================================================
    Constants
==============================================================================================*/

#define GLYPH_FIRST   32u
#define GLYPH_LAST   126u
#define GLYPH_MAX    ( GLYPH_LAST - GLYPH_FIRST + 1u )   /* 95 */

#define GLYPH_PAD     1          /* gap between packed rects (prevents filter bleed) */
#define ATLAS_W       512
#define ATLAS_H       512

/*==============================================================================================
    Internal types
==============================================================================================*/

/* Rasterized glyph before atlas placement. */
typedef struct
{
    uint32_t  codepoint;
    uint8_t*  bitmap;    /* malloc'd row-major pixels (w * h bytes); NULL for empty glyphs */
    uint32_t  w;
    uint32_t  h;
    int16_t   bearing_x;
    int16_t   bearing_y;
    uint16_t  advance;
} raw_glyph_t;

/*==============================================================================================
    Static storage (256 KB atlas lives in BSS, not the stack)
==============================================================================================*/

static raw_glyph_t       s_raw[ GLYPH_MAX ];
static uint8_t           s_atlas[ ATLAS_H * ATLAS_W ];
static orb_font_glyph_t  s_out_glyphs[ GLYPH_MAX ];
static stbrp_node        s_nodes[ ATLAS_W ];

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    if ( argc != 4 )
    {
        fprintf( stderr, "usage: font_tool <input.ttf> <size_px> <output.orb_font>\n" );
        return 1;
    }

    const char* ttf_path = argv[ 1 ];
    int         size_px  = atoi( argv[ 2 ] );
    const char* out_path = argv[ 3 ];

    if ( size_px < 6 || size_px > 256 )
    {
        fprintf( stderr, "error: size_px must be 6..256\n" );
        return 1;
    }

    /*------------------------------------------------------------------------------------------
        Pass 1 -- rasterize all glyphs via FreeType into heap bitmaps.
    ------------------------------------------------------------------------------------------*/

    FT_Library ft;
    if ( FT_Init_FreeType( &ft ) )
    {
        fprintf( stderr, "error: FT_Init_FreeType failed\n" );
        return 1;
    }

    FT_Face face;
    if ( FT_New_Face( ft, ttf_path, 0, &face ) )
    {
        fprintf( stderr, "error: cannot load font '%s'\n", ttf_path );
        FT_Done_FreeType( ft );
        return 1;
    }

    FT_Set_Pixel_Sizes( face, 0, (FT_UInt)size_px );

    /* global metrics -- FreeType uses 26.6 fixed-point, >> 6 converts to integer pixels */
    int32_t ascent   = (int32_t)( face->size->metrics.ascender  >> 6 );
    int32_t descent  = (int32_t)( face->size->metrics.descender >> 6 );
    int32_t line_gap = (int32_t)( face->size->metrics.height    >> 6 ) - ascent + descent;

    uint32_t raw_count = 0;

    for ( uint32_t cp = GLYPH_FIRST; cp <= GLYPH_LAST; ++cp )
    {
        FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)cp );
        if ( FT_Load_Glyph( face, glyph_idx, FT_LOAD_RENDER ) )
            continue;

        FT_GlyphSlot   g  = face->glyph;
        raw_glyph_t*   r  = &s_raw[ raw_count++ ];

        r->codepoint = cp;
        r->w         = g->bitmap.width;
        r->h         = g->bitmap.rows;
        r->bearing_x = (int16_t)( g->metrics.horiBearingX >> 6 );
        r->bearing_y = (int16_t)( g->metrics.horiBearingY >> 6 );
        r->advance   = (uint16_t)( g->advance.x >> 6 );
        r->bitmap    = NULL;

        /* copy bitmap pixels; pitch may exceed width due to alignment */
        if ( r->w > 0 && r->h > 0 )
        {
            r->bitmap = (uint8_t*)malloc( r->w * r->h );
            if ( !r->bitmap )
            {
                fprintf( stderr, "error: out of memory\n" );
                FT_Done_Face( face );
                FT_Done_FreeType( ft );
                return 1;
            }
            for ( uint32_t row = 0; row < r->h; ++row )
            {
                const uint8_t* src = g->bitmap.buffer + row * (uint32_t)g->bitmap.pitch;
                memcpy( r->bitmap + row * r->w, src, r->w );
            }
        }
    }

    FT_Done_Face( face );
    FT_Done_FreeType( ft );

    /*------------------------------------------------------------------------------------------
        Pass 2 -- pack glyph rects into the atlas using stb_rect_pack.
        GLYPH_PAD is added to each rect dimension so there is a 1-pixel gap
        between neighbours, preventing bilinear filter bleed.
    ------------------------------------------------------------------------------------------*/

    stbrp_context pack_ctx;
    stbrp_init_target( &pack_ctx, ATLAS_W, ATLAS_H, s_nodes, ATLAS_W );

    stbrp_rect rects[ GLYPH_MAX ];
    for ( uint32_t i = 0; i < raw_count; ++i )
    {
        rects[ i ].id         = (int)i;
        rects[ i ].w          = (stbrp_coord)( s_raw[ i ].w + GLYPH_PAD );
        rects[ i ].h          = (stbrp_coord)( s_raw[ i ].h + GLYPH_PAD );
        rects[ i ].was_packed = 0;
    }

    if ( !stbrp_pack_rects( &pack_ctx, rects, (int)raw_count ) )
    {
        fprintf( stderr, "error: atlas %dx%d too small for %u glyphs at %d px\n",
                 ATLAS_W, ATLAS_H, raw_count, size_px );
        for ( uint32_t i = 0; i < raw_count; ++i )
            free( s_raw[ i ].bitmap );
        return 1;
    }

    /*------------------------------------------------------------------------------------------
        Pass 3 -- blit bitmaps into atlas and build output glyph table.
    ------------------------------------------------------------------------------------------*/

    memset( s_atlas, 0, sizeof( s_atlas ) );

    for ( uint32_t i = 0; i < raw_count; ++i )
    {
        raw_glyph_t*      r  = &s_raw[ i ];
        stbrp_rect*       rc = &rects[ i ];
        orb_font_glyph_t* og = &s_out_glyphs[ i ];

        og->codepoint = r->codepoint;
        og->atlas_x   = (uint16_t)rc->x;
        og->atlas_y   = (uint16_t)rc->y;
        og->w         = (uint16_t)r->w;
        og->h         = (uint16_t)r->h;
        og->bearing_x = r->bearing_x;
        og->bearing_y = r->bearing_y;
        og->advance   = r->advance;
        og->_pad      = 0;

        if ( r->bitmap )
        {
            for ( uint32_t row = 0; row < r->h; ++row )
            {
                const uint8_t* src = r->bitmap + row * r->w;
                uint8_t*       dst = s_atlas + ( (uint32_t)rc->y + row ) * ATLAS_W + (uint32_t)rc->x;
                memcpy( dst, src, r->w );
            }
            free( r->bitmap );
            r->bitmap = NULL;
        }
    }

    /*------------------------------------------------------------------------------------------
        Write .orb_font file.
    ------------------------------------------------------------------------------------------*/

    FILE* f = fopen( out_path, "wb" );
    if ( !f )
    {
        fprintf( stderr, "error: cannot open '%s' for writing\n", out_path );
        return 1;
    }

    orb_font_header_t hdr;
    memset( &hdr, 0, sizeof( hdr ) );
    hdr.magic       = ORB_FONT_MAGIC;
    hdr.version     = ORB_FONT_VERSION;
    hdr.atlas_w     = ATLAS_W;
    hdr.atlas_h     = ATLAS_H;
    hdr.font_size   = (uint32_t)size_px;
    hdr.ascent      = ascent;
    hdr.descent     = descent;
    hdr.line_gap    = line_gap;
    hdr.glyph_count = raw_count;

    fwrite( &hdr,         sizeof( hdr ),              1,         f );
    fwrite( s_out_glyphs, sizeof( orb_font_glyph_t ), raw_count, f );
    fwrite( s_atlas,      1,                           ATLAS_W * ATLAS_H, f );
    fclose( f );

    printf( "font_tool: %u glyphs, %dx%d atlas, %d px -> '%s'\n",
            raw_count, ATLAS_W, ATLAS_H, size_px, out_path );
    return 0;
}
