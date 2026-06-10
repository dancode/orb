/*==============================================================================================

    tools/font_tool/font_tool.c -- Offline font atlas baker.

    Rasterizes a TTF/OTF font at a given pixel size using FreeType, packs the
    glyph bitmaps into a texture atlas with stb_rect_pack, and writes either:
        .orb_font  -- binary atlas file the engine loads at runtime
        .c         -- C source file with a bit-packed glyph table matching
                      the bitmap_font_def_t format in imgui_font_builtin.c;
                      drop the output into imgui_font_builtin.c as a new builtin.

    Usage:
        font_tool.exe <input.ttf> <size_px> <output.orb_font>
        font_tool.exe <input.ttf> <size_px> <output.c>

    Bakes ASCII printable range U+0020..U+007F (96 glyphs; DEL synthesized as solid block).
    .orb_font output atlas is always 512x512, R8 grayscale.
    .c output requires a monospace TTF; cell width must be <= 16 px (fits in u16 row).

    Link deps: freetype.lib (import lib for freetype.dll)

    Fonts are usually in: C:\WINDOWS\FONTS\<font_name>.ttf

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
    write_c_export -- emit a .c file with a bit-packed glyph table.

    Produces a static const u8/u16 array [96][cell_h] and a bitmap_font_def_t
    initializer compatible with imgui_font_builtin.c.  Each row is packed with
    bit 0 = leftmost pixel (column 0); same convention as the hand-coded builtins.
    DEL (0x7F, index 95) is synthesized as a solid filled block.

    Returns 0 on success, 1 on error.
==============================================================================================*/

/* Maximum atlas pixel count that bitmap_atlas_init can handle (fixed stack buffer). */
#define BITMAP_ATLAS_PIXELS_MAX  ( 256u * 128u )

static int
write_c_export( const char* out_path, int size_px,
                int32_t ascent, int32_t descent, uint32_t raw_count )
{
    /* Derive a C-safe identifier from the output filename stem (e.g. "consola_12" from
       "path/consola_12.c") so that generated symbols are unique across multiple exports. */

    const char* base = out_path;
    for ( const char* p = out_path; *p; ++p )
        if ( *p == '/' || *p == '\\' ) base = p + 1;

    size_t stem_len = strlen( base );
    if ( stem_len >= 2 && base[ stem_len - 2 ] == '.'
         && ( base[ stem_len - 1 ] == 'c' || base[ stem_len - 1 ] == 'C' ) )
        stem_len -= 2;
    if ( stem_len >= 60 ) stem_len = 60;

    char stem[ 64 ];
    for ( size_t si = 0; si < stem_len; ++si )
    {
        char ch  = base[ si ];
        stem[si] = ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' )
                     || ( ch >= '0' && ch <= '9' ) ) ? ch : '_';
    }
    stem[ stem_len ] = '\0';

    /* Cell dimensions from global FreeType metrics and max glyph advance. */

    int32_t  cell_h_s = ascent - descent;  /* signed; descent is negative */
    uint32_t cell_w   = 0;
    for ( uint32_t i = 0; i < raw_count; ++i )
        if ( s_raw[ i ].advance > cell_w ) cell_w = s_raw[ i ].advance;

    if ( cell_w == 0 )
    {
        fprintf( stderr, "error: all glyph advances are zero\n" );
        return 1;
    }
    if ( cell_w > 16 )
    {
        fprintf( stderr, "error: cell_w %u > 16 -- use a smaller size or a narrower font\n", cell_w );
        return 1;
    }
    if ( cell_h_s <= 0 || cell_h_s > 64 )
    {
        fprintf( stderr, "error: cell_h %d out of range 1..64\n", cell_h_s );
        return 1;
    }

    /* Atlas size is hard coded 16 in a row, 6 columns = 96 characters */

    uint32_t cell_h      = (uint32_t)cell_h_s;
    uint32_t row_stride = ( cell_w <= 8 ) ? 1u : 2u; /* cell_w in bytes ( 1/2 = 8/16 bits) */
    uint32_t glyphs_row  = 16u;                      /* 16 glyphs per row = 16x16 = 256w */
    uint32_t glyph_count = 96u;                      /* ASCII 32..127 */
    uint32_t atlas_w     = glyphs_row * cell_w;      /* 16 cols = 16 glyphs/row * cell_w px/glyph */
    uint32_t atlas_h     = 6u * cell_h;              /* ceil(96/16) = 6 rows */

    /* Reject atlases that would overflow the fixed pixel buffer in bitmap_atlas_init. */
    if ( atlas_w * atlas_h > BITMAP_ATLAS_PIXELS_MAX )
    {
        fprintf( stderr, "error: atlas %ux%u (%u px) exceeds built-in pixel buffer limit "
                         "(%u px); use a smaller font size\n",
                 atlas_w, atlas_h, atlas_w * atlas_h, BITMAP_ATLAS_PIXELS_MAX );
        return 1;
    }

    /* Flat bit-packed table: data[ glyph * cell_h + row ] is row_stride bytes. */

    uint8_t* table = (uint8_t*)calloc( glyph_count * cell_h * row_stride, 1 );
    if ( !table )
    {
        fprintf( stderr, "error: out of memory\n" );
        return 1;
    }

    for ( uint32_t g = 0; g < glyph_count; ++g )
    {
        uint32_t cp = 32u + g;

        /* DEL (0x7F): solid filled block as a visual sentinel. */
        if ( cp == 127u )
        {
            uint32_t mask = ( row_stride == 1 ) ? 0xFFu : ( ( 1u << cell_w ) - 1u );
            for ( uint32_t r = 0; r < cell_h; ++r )
            {
                uint8_t* row = table + ( g * cell_h + r ) * row_stride;
                row[ 0 ] = (uint8_t)( mask & 0xFFu );
                if ( row_stride > 1u ) row[ 1 ] = (uint8_t)( mask >> 8 );
            }
            continue;
        }

        /* Find the raw glyph for this codepoint. */
        raw_glyph_t* rg = NULL;
        for ( uint32_t i = 0; i < raw_count; ++i )
        {
            if ( s_raw[ i ].codepoint == cp ) { rg = &s_raw[ i ]; break; }
        }

        /* Space and whitespace glyphs have no bitmap; leave cell zeroed. */
        if ( !rg || !rg->bitmap ) continue;

        /* Place bitmap: x_off = bearing_x, y_off = ascent - bearing_y (top of cell = row 0). */
        int32_t x_off = (int32_t)rg->bearing_x;
        int32_t y_off = ascent - (int32_t)rg->bearing_y;

        for ( uint32_t r = 0; r < rg->h; ++r )
        {
            int32_t cell_row = y_off + (int32_t)r;
            if ( cell_row < 0 || cell_row >= (int32_t)cell_h ) continue;

            uint8_t* row = table + ( g * cell_h + (uint32_t)cell_row ) * row_stride;

            for ( uint32_t c = 0; c < rg->w; ++c )
            {
                int32_t cell_col = x_off + (int32_t)c;
                if ( cell_col < 0 || cell_col >= (int32_t)cell_w ) continue;

                /* FreeType grayscale: threshold at 128 -> 1 bit. */
                if ( rg->bitmap[ r * rg->w + c ] >= 128u )
                {
                    uint32_t bit = 1u << (uint32_t)cell_col;
                    row[ 0 ] |= (uint8_t)( bit & 0xFFu );
                    if ( row_stride > 1u ) row[ 1 ] |= (uint8_t)( bit >> 8 );
                }
            }
        }
    }

    /* Open output file. */
    FILE* f = fopen( out_path, "w" );
    if ( !f )
    {
        fprintf( stderr, "error: cannot open '%s' for writing\n", out_path );
        free( table );
        return 1;
    }

    /* Header comment. */
    fprintf( f, "/*------------------------------------------------------------------\n" );
    fprintf( f, "    Generated by font_tool -- %d px bitmap font export.\n", size_px );
    fprintf( f, "    cell: %u x %u px  atlas: %u x %u px  row_stride: %u\n",
                     cell_w, cell_h, atlas_w, atlas_h, row_stride );
    fprintf( f, "    Drop into imgui_font_builtin.c and add a bitmap_font_t instance.\n" );
    fprintf( f, "------------------------------------------------------------------*/\n\n" );

    /* Data array. */
    const char* elem_type = ( row_stride == 1 ) ? "u8" : "u16";
    const char* hex_fmt   = ( row_stride == 1 ) ? "0x%02X" : "0x%03X";
    fprintf( f, "static const %s s_font_%s_data[ 96 ][ %u ] =\n{\n", elem_type, stem, cell_h );

    for ( uint32_t g = 0; g < glyph_count; ++g )
    {
        uint32_t cp = 32u + g;

        /* Format the character label for the comment. */
        char label[ 8 ];
        if      ( cp == '\'' ) { label[ 0 ] = '\\'; label[ 1 ] = '\''; label[ 2 ] = '\0'; }
        else if ( cp == '\\' ) { label[ 0 ] = '\\'; label[ 1 ] = '\\'; label[ 2 ] = '\0'; }
        else if ( cp == 127u ) { label[ 0 ] = 'D';  label[ 1 ] = 'E';  label[ 2 ] = 'L'; label[ 3 ] = '\0'; }
        else                   { label[ 0 ] = (char)cp; label[ 1 ] = '\0'; }

        fprintf( f, "    /* 0x%02X '%s' */ {", cp, label );

        for ( uint32_t r = 0; r < cell_h; ++r )
        {
            uint8_t* row = table + ( g * cell_h + r ) * row_stride;
            uint32_t val = row[ 0 ];
            if ( row_stride > 1u ) val |= (uint32_t)row[ 1 ] << 8;
            fprintf( f, hex_fmt, val );
            if ( r + 1u < cell_h ) fprintf( f, "," );
        }

        fprintf( f, " },\n" );
    }
    fprintf( f, "};\n\n" );

    /* bitmap_font_def_t initializer. */
    const char* data_fmt = ( row_stride == 1 )
        ? "    .data        = &s_font_%s_data[ 0 ][ 0 ],\n"
        : "    .data        = ( const u8* )s_font_%s_data,\n";

    fprintf( f, "static const bitmap_font_def_t s_def_%s = {\n", stem );
    fprintf( f, "    .atlas_w     = %u,\n", atlas_w );
    fprintf( f, "    .atlas_h     = %u,\n", atlas_h );
    fprintf( f, "    .glyph_w     = %u,\n", cell_w );
    fprintf( f, "    .glyph_h     = %u,\n", cell_h );
    fprintf( f, "    .glyphs_row  = %u,\n", glyphs_row );
    fprintf( f, "    .glyph_count = %u,\n", glyph_count );
    fprintf( f, "    .row_stride  = %u,\n", row_stride );
    fprintf( f, data_fmt, stem );
    fprintf( f, "    .debug_name  = \"imgui_builtin_%s\",\n", stem );
    fprintf( f, "};\n" );

    fclose( f );
    free( table );

    printf( "font_tool: stem '%s', 96 glyphs, cell %ux%u, atlas %ux%u, %s rows -> '%s'\n",
            stem, cell_w, cell_h, atlas_w, atlas_h,
            ( row_stride == 1 ) ? "u8" : "u16", out_path );

    return 0; /* success! */
}

/*==============================================================================================
    path_has_c_ext -- returns 1 if path ends in ".c" (case-insensitive).
==============================================================================================*/

static int
path_has_c_ext( const char* path )
{
    size_t n = strlen( path );
    return n >= 2 && path[ n - 2 ] == '.' && ( path[ n - 1 ] == 'c' || path[ n - 1 ] == 'C' );
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

        if ( has_dir )
        {
            out_path = out_arg;
        }
        else
        {
            int n = snprintf( s_out_buf, sizeof( s_out_buf ), "fonts\\%s", out_arg );
            if ( n <= 0 || n >= (int)sizeof( s_out_buf ) )
            {
                fprintf( stderr, "error: output path too long\n" );
                return 1;
            }
            out_path = s_out_buf;
        }
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

    FT_Set_Pixel_Sizes( face, 0, (FT_UInt)size_px );

    /* global metrics -- FreeType uses 26.6 fixed-point, >> 6 converts to integer pixels */
    int32_t ascent   = (int32_t)( face->size->metrics.ascender  >> 6 );
    int32_t descent  = (int32_t)( face->size->metrics.descender >> 6 );
    int32_t line_gap = (int32_t)( face->size->metrics.height    >> 6 ) - ascent + descent;

    uint32_t raw_count = 0;

    /* .c export: mono hinting snaps stems to whole pixels for consistent 1-bit output.
       .orb_font: keep full grayscale for smooth atlas rendering at runtime. */
    int      is_c_export = path_has_c_ext( out_path );
    FT_Int32 load_flags  = is_c_export ? ( FT_LOAD_RENDER | FT_LOAD_TARGET_MONO )
                                       : FT_LOAD_RENDER;

    for ( uint32_t cp = GLYPH_FIRST; cp <= GLYPH_LAST; ++cp )
    {
        FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)cp );
        if ( FT_Load_Glyph( face, glyph_idx, load_flags ) )
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
                if ( is_c_export )
                {
                    /* Mono bitmap: 1 bit per pixel, MSB = col 0, packed by pitch bytes per row.
                       Unpack to 1 byte per pixel (0 or 255) for uniform downstream handling. */
                    for ( uint32_t col = 0; col < r->w; ++col )
                        dst[ col ] = ( src[ col >> 3 ] & ( 0x80u >> ( col & 7u ) ) ) ? 255u : 0u;
                }
                else
                {
                    memcpy( dst, src, r->w );
                }
            }
        }
    }

    FT_Done_Face( face );
    FT_Done_FreeType( ft );

    /*------------------------------------------------------------------------------------------
        C export path -- write bit-packed builtin font source and exit early.
    ------------------------------------------------------------------------------------------*/

    if ( is_c_export )
    {
        int ret = write_c_export( out_path, size_px, ascent, descent, raw_count );
        for ( uint32_t i = 0; i < raw_count; ++i )
            free( s_raw[ i ].bitmap );
        return ret;
    }

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
