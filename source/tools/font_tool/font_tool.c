/*==============================================================================================

    tools/font_tool/font_tool.c -- Offline font atlas baker.

    Rasterizes a TTF/OTF font at a given pixel size using FreeType, packs the
    glyph bitmaps into a texture atlas with stb_rect_pack, and writes an
    .orb_font binary atlas file the engine loads at runtime.

    Usage:
        font_tool.exe <input.ttf> <size_px> <output.orb_font>
        font_tool.exe info [<file.orb_font> | <dir>]...   -- print .orb_font header internals

    Bakes the ASCII printable range U+0020 (space) .. U+007E (tilde), 95 glyphs.
    .orb_font output atlas is R8 grayscale, sized to the smallest power-of-two square that fits
    every packed glyph.  Rasterization is the only font_tool-specific step -- packing, atlas
    blit and the .orb_font write are shared with the runtime baker via dev_font_bake_write().

    Link deps: freetype.lib (import lib for freetype.dll), dev_font (shared bake back-end)

    Fonts are usually in: C:\WINDOWS\FONTS\<font_name>.ttf

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "tools/font_tool/orb_font.h"
#include "developer/dev_font/dev_font.h"   /* shared path resolver, output dirs, bake back-end */
#include "engine/sys/sys_host.h"           /* sys_dir_make */

/*==============================================================================================
    Constants
==============================================================================================*/

#if OS_WINDOWS
    #define OUT_SEP "\\"
#else
    #define OUT_SEP "/"
#endif

/* The baked codepoint range and glyph count come from the format contract (orb_font.h):
   ORB_FONT_CP_FIRST / ORB_FONT_CP_LAST / ORB_FONT_CP_COUNT. */

/*==============================================================================================
    Static storage -- rasterized glyphs handed to the shared back-end (BSS, not the stack).
==============================================================================================*/

static dev_font_glyph_t  s_glyphs[ ORB_FONT_CP_COUNT ];

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
    info subcommand -- read .orb_font headers and print their internals as a table.

    Read-only diagnostic companion to the bake path: `font_tool info [<file.orb_font> | <dir>]...`.
    With no argument it lists the engine font dir (assets/font); a .orb_font argument prints that one
    file; any other argument is treated as a directory and globbed for *.orb_font.
==============================================================================================*/

typedef struct { int count; } info_ctx_t;

/* Column header (with a one-line legend), printed once above the rows. */
static void
info_print_header( void )
{
    printf( "\n%-40s %3s %5s %5s %5s %5s %5s %5s %7s\n",
            "file", "ver", "aw", "ah", "size", "asc", "desc", "gap", "glyphs" );
}

/* Read one .orb_font header and print its row (or a short note if it is not a valid atlas). */
static void
info_print_file( const char* label, const char* path )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
    {
        printf( "%-40s  (cannot open)\n", label );
        return;
    }
    orb_font_header_t h;
    size_t got = fread( &h, 1, sizeof( h ), f );
    fclose( f );

    if ( got != sizeof( h ) || h.magic != ORB_FONT_MAGIC )
    {
        printf( "%-40s  (not an .orb_font)\n", label );
        return;
    }
    printf( "%-40s %3u %5u %5u %5u %5d %5d %5d %7u\n",
            label, h.version, h.atlas_w, h.atlas_h, h.font_size,
            h.ascent, h.descent, h.line_gap, h.glyph_count );
}

/* sys_file_glob callback: print each matched file, keep going. */
static bool
info_glob_cb( const char* filename, const char* full_path, void* userdata )
{
    info_ctx_t* c = (info_ctx_t*)userdata;
    info_print_file( filename, full_path );
    ++c->count;
    return true;
}

/* basename of a path (after the last '/' or '\\'). */
static const char*
path_base( const char* p )
{
    const char* base = p;
    for ( const char* q = p; *q; ++q )
        if ( *q == '/' || *q == '\\' )
            base = q + 1;
    return base;
}

static int
run_info( int npaths, char** paths )
{
    /* Initialize dev_font up front so its one-line init banner prints before the table, not
       between the column titles and the rows. */
    dev_font_init( NULL );

    info_print_header();
    info_ctx_t ctx = { 0 };

    if ( npaths == 0 )
    {
        /* No argument -- default to the engine's font output dir (assets/font). */
        char dir[ 512 ];
        if ( dev_font_dir( dir, sizeof( dir ) ) )
            sys_file_glob( dir, "*.orb_font", info_glob_cb, &ctx );
    }
    else
    {
        for ( int i = 0; i < npaths; ++i )
        {
            if ( path_has_orb_font_ext( paths[ i ] ) )
            {
                info_print_file( path_base( paths[ i ] ), paths[ i ] );
                ++ctx.count;
            }
            else
            {
                sys_file_glob( paths[ i ], "*.orb_font", info_glob_cb, &ctx );
            }
        }
    }

    if ( ctx.count == 0 )
        printf( "(no .orb_font files found)\n" );
    return 0;
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    /* Subcommand: `info` prints .orb_font header internals (read-only diagnostic). */
    if ( argc >= 2 && strcmp( argv[ 1 ], "info" ) == 0 )
        return run_info( argc - 2, argv + 2 );

    if ( argc < 3 || argc > 4 )
    {
        fprintf( stderr, "usage: font_tool <input.ttf | \"Font Name\"> <size_px> [output.orb_font]\n" );
        fprintf( stderr, "       font_tool info [<file.orb_font> | <dir>]...   (print header internals)\n" );
        fprintf( stderr, "       input may be a path, a bare filename, or an installed font name\n" );
        fprintf( stderr, "       output defaults to assets/font/<name>_<size>px.orb_font\n" );
        return 1;
    }

    const char* ttf_path = argv[ 1 ];
    int         size_px  = atoi( argv[ 2 ] );

    /* Resolve a bare filename or friendly font name ("Cascadia Mono") to an absolute TTF path,
       searching assets/font_source/ and the OS fonts -- the same resolver the runtime stb baker
       uses (dev_font).  A path with a directory separator is accepted as-is. */

    dev_font_init( NULL );

    static char s_ttf_abs[ 512 ];
    if ( !dev_font_resolve( ttf_path, s_ttf_abs, sizeof( s_ttf_abs ) ) )
    {
        fprintf( stderr, "error: %s\n", dev_font_last_error() );
        return 1;
    }
    ttf_path = s_ttf_abs;

    if ( size_px < 6 || size_px > 256 )
    {
        fprintf( stderr, "error: size_px must be 6..256\n" );
        return 1;
    }

    /* Final (FreeType) bakes land in assets/font/, parallel to the assets/font_cache/ that the
       runtime stb baker (dev_font_get) uses.  dev_font owns both paths so the two stay aligned. */

    char font_dir[ 512 ];
    if ( !dev_font_dir( font_dir, sizeof( font_dir ) ) )
    {
        fprintf( stderr, "error: dev_font not initialized\n" );
        return 1;
    }

    /* Output rules:
         - an output arg with a directory component is used as-is;
         - a bare-filename output arg is redirected into assets/font/;
         - no output arg derives assets/font/<stem>_<size>px.orb_font (matches the stb cache name).
       A missing .orb_font extension is appended. */

    static char s_out_buf[ 512 ];
    const char* out_arg = ( argc == 4 ) ? argv[ 3 ] : NULL;
    const char* out_path;

    if ( out_arg )
    {
        int has_dir = 0;
        for ( const char* p = out_arg; *p; ++p )
            if ( *p == '/' || *p == '\\' ) { has_dir = 1; break; }

        int n = has_dir ? snprintf( s_out_buf, sizeof( s_out_buf ), "%s", out_arg )
                        : snprintf( s_out_buf, sizeof( s_out_buf ), "%s" OUT_SEP "%s", font_dir, out_arg );
        if ( n <= 0 || n >= (int)sizeof( s_out_buf ) )
        {
            fprintf( stderr, "error: output path too long\n" );
            return 1;
        }

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
        /* No output given -- derive stem from the resolved input TTF. */

        const char* base = ttf_path;
        for ( const char* p = ttf_path; *p; ++p )
            if ( *p == '/' || *p == '\\' ) base = p + 1;

        size_t stem_len = strlen( base );
        for ( size_t i = stem_len; i-- > 0; )
            if ( base[ i ] == '.' ) { stem_len = i; break; }

        int n = snprintf( s_out_buf, sizeof( s_out_buf ),
                          "%s" OUT_SEP "%.*s_%dpx.orb_font", font_dir, (int)stem_len, base, size_px );
        if ( n <= 0 || n >= (int)sizeof( s_out_buf ) )
        {
            fprintf( stderr, "error: derived output path too long\n" );
            return 1;
        }
        out_path = s_out_buf;
    }

    /* Create the output directory if it does not exist (parent of out_path). */

    char out_dir[ 512 ];
    snprintf( out_dir, sizeof( out_dir ), "%s", out_path );
    for ( int i = (int)strlen( out_dir ) - 1; i >= 0; --i )
    {
        if ( out_dir[ i ] == '/' || out_dir[ i ] == '\\' )
        {
            out_dir[ i ] = '\0';
            sys_dir_make( out_dir );
            break;
        }
    }

    /*------------------------------------------------------------------------------------------
        Rasterize all glyphs via FreeType into the shared dev_font_glyph_t scratch, then hand off
        to dev_font_bake_write() for packing, atlas blit and the .orb_font write -- the same
        back-end the runtime stb baker uses, so both produce byte-identical output.
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

    memset( s_glyphs, 0, sizeof( s_glyphs ) );
    uint32_t raw_count = 0;

    for ( uint32_t cp = ORB_FONT_CP_FIRST; cp <= ORB_FONT_CP_LAST; ++cp )
    {
        FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)cp );
        if ( FT_Load_Glyph( face, glyph_idx, FT_LOAD_RENDER ) )
            continue;

        FT_GlyphSlot      g = face->glyph;
        dev_font_glyph_t* r = &s_glyphs[ raw_count++ ];

        r->codepoint = cp;
        r->w         = (int)g->bitmap.width;
        r->h         = (int)g->bitmap.rows;
        r->bearing_x = (int)( g->metrics.horiBearingX >> 6 );
        r->bearing_y = (int)( g->metrics.horiBearingY >> 6 );   // FT convention: positive = above baseline
        r->advance   = (int)( g->advance.x >> 6 );

        /* copy bitmap pixels; pitch may exceed width due to alignment */
        if ( r->w > 0 && r->h > 0 )
        {
            r->bitmap = (uint8_t*)malloc( (size_t)r->w * r->h );
            if ( !r->bitmap )
            {
                fprintf( stderr, "error: out of memory\n" );
                FT_Done_Face( face );
                FT_Done_FreeType( ft );
                return 1;
            }
            for ( int row = 0; row < r->h; ++row )
            {
                const uint8_t* src = g->bitmap.buffer + (size_t)row * (uint32_t)g->bitmap.pitch;
                uint8_t*       dst = r->bitmap + (size_t)row * r->w;
                memcpy( dst, src, (size_t)r->w );
            }
        }
    }

    FT_Done_Face( face );
    FT_Done_FreeType( ft );

    bool ok = dev_font_bake_write( out_path, s_glyphs, raw_count,
                                   ascent, descent, line_gap, size_px, ttf_path );
    if ( !ok )
        fprintf( stderr, "error: %s\n", dev_font_last_error() );

    for ( uint32_t i = 0; i < raw_count; ++i )
        free( s_glyphs[ i ].bitmap );

    return ok ? 0 : 1;
}

/*============================================================================================*/
// clang-format on