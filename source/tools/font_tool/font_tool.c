/*==============================================================================================

    tools/font_tool/font_tool.c -- Offline font atlas baker.

    Rasterizes a TTF/OTF font at a given pixel size using FreeType, packs the
    glyph bitmaps into a texture atlas with stb_rect_pack, and writes an
    .orb_font binary atlas file the engine loads at runtime.

    Usage:
        font_tool.exe <input.ttf> <size_px> <output.orb_font>

    Bakes ASCII printable range U+0020..U+007F (96 glyphs; DEL synthesized as solid block).
    .orb_font output atlas is always 512x512, R8 grayscale.

    Link deps: freetype.lib (import lib for freetype.dll)

    Fonts are usually in: C:\WINDOWS\FONTS\<font_name>.ttf

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include "tools/font_tool/stb_rect_pack.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include "tools/font_tool/orb_font.h"

/*==============================================================================================
    Constants
==============================================================================================*/

#define GLYPH_FIRST   32u       // ASCII space; codepoints below this are control chars.
#define GLYPH_LAST    126u      // ASCII tilde; codepoints above this are non-ASCII.

#define GLYPH_MAX    ( GLYPH_LAST - GLYPH_FIRST + 1u )   /* 95 */

#define GLYPH_PAD     1         // gap between packed rects (prevents filter bleed)
#define ATLAS_W       512       // atlas width in pixels
#define ATLAS_H       512       // atlas height in pixels

/*==============================================================================================
    Internal types
==============================================================================================*/

/* Rasterized glyph before atlas placement. */

typedef struct raw_glyph_s
{
    uint32_t  codepoint;        // Unicode codepoint
    uint8_t*  bitmap;           // malloc'd row-major pixels (w * h bytes); NULL for empty glyphs
    uint32_t  w;                // bitmap width in pixels
    uint32_t  h;                // bitmap height in pixels
    int16_t   bearing_x;        // cursor-to-left-edge offset in pixels
    int16_t   bearing_y;        // baseline-to-top-edge offset in pixels (positive up)
    uint16_t  advance;          // horizontal advance in pixels

} raw_glyph_t;

/*==============================================================================================
    Static storage (256 KB atlas lives in BSS, not the stack)
==============================================================================================*/

static raw_glyph_t       s_raw          [ GLYPH_MAX ];
static orb_font_glyph_t  s_out_glyphs   [ GLYPH_MAX ];
static stbrp_node        s_nodes        [ ATLAS_W ];
static uint8_t           s_atlas        [ ATLAS_W * ATLAS_H ];

/*==============================================================================================
    path_has_orb_font_ext -- returns 1 if path ends in ".orb_font" (case-insensitive).
==============================================================================================*/

static int
path_has_orb_font_ext( const char* path )
{
    static const char ext[] = ".orb_font";
    size_t            n      = strlen( path );
    size_t            e      = sizeof( ext ) - 1;
    if ( n < e )
        return 0;

    const char* tail = path + ( n - e );
    for ( size_t i = 0; i < e; ++i )
        if ( tolower( (unsigned char)tail[ i ] ) != ext[ i ] )
            return 0;
    return 1;
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    if ( argc < 3 || argc > 4 )
    {
        fprintf( stderr, "usage: font_tool <input.ttf> <size_px> [output.orb_font]\n" );
        fprintf( stderr, "       output defaults to fonts\\<name>.orb_font\n" );
        return 1;
    }

    const char* ttf_path = argv[ 1 ];
    int         size_px  = atoi( argv[ 2 ] );

    /* If the output path is a bare filename (no directory separator), redirect it into fonts\.
       If omitted entirely, derive the filename from the input TTF stem. */

    static char s_out_buf[ 512 ];
    const char* out_arg = ( argc == 4 ) ? argv[ 3 ] : NULL;
    const char* out_path;

    if ( out_arg )
    {
        /* Check whether out_arg already contains a directory component. */
        int has_dir = 0;
        for ( const char* p = out_arg; *p; ++p )
            if ( *p == '/' || *p == '\\' ) { has_dir = 1; break; }

        /* Paths with a directory are used as-is; bare filenames are redirected into fonts\. */
        int n = has_dir ? snprintf( s_out_buf, sizeof( s_out_buf ), "%s", out_arg )
                        : snprintf( s_out_buf, sizeof( s_out_buf ), "fonts\\%s", out_arg );
        if ( n <= 0 || n >= (int)sizeof( s_out_buf ) )
        {
            fprintf( stderr, "error: output path too long\n" );
            return 1;
        }

        /* Default the extension to .orb_font when the user omitted a recognized one. */
        if ( !path_has_orb_font_ext( s_out_buf ) )
        {
            int m = snprintf( s_out_buf + n, sizeof( s_out_buf ) - (size_t)n, ".orb_font" );
            if ( m <= 0 || m >= (int)sizeof( s_out_buf ) - n )
            {
                fprintf( stderr, "error: output path too long\n" );
                return 1;
            }
        }
        out_path = s_out_buf;
    }
    else
    {
        /* No output given -- derive stem from input TTF. */

        const char* base = ttf_path;
        for ( const char* p = ttf_path; *p; ++p )
            if ( *p == '/' || *p == '\\' ) base = p + 1;

        size_t stem_len = strlen( base );
        for ( size_t i = stem_len; i-- > 0; )
            if ( base[ i ] == '.' ) { stem_len = i; break; }

        int n = snprintf( s_out_buf, sizeof( s_out_buf ),
                          "fonts\\%.*s.orb_font", (int)stem_len, base );
        if ( n <= 0 || n >= (int)sizeof( s_out_buf ) )
        {
            fprintf( stderr, "error: derived output path too long\n" );
            return 1;
        }
        out_path = s_out_buf;
    }

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

    /* Set pixel size; 0 for width means "same as height". */
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
                uint8_t*       dst = r->bitmap + row * r->w;
                memcpy( dst, src, r->w );
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

/*============================================================================================*/
// clang-format on