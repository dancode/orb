/*==============================================================================================

    dev_font.c -- Developer runtime font baker.

    Unity build entry for the dev_font static library.  Defines STB_RECT_PACK_IMPLEMENTATION
    and STB_TRUETYPE_IMPLEMENTATION here so no other TU pulls them in.

    Pipeline (identical output format to font_tool.exe):
        Pass 1 -- rasterize glyphs with stb_truetype into heap bitmaps.
        Pass 2 -- pack glyph rects with stb_rect_pack (GLYPH_PAD gap between each).
        Pass 3 -- blit bitmaps into a 512x512 R8 atlas; write orb_font_header_t + glyph
                  records + pixels to assets/font_cache/.

    Cache invalidation: sys_file_time() is compared between the source TTF and the cached
    .orb_font.  A cache hit skips all three passes.

==============================================================================================*/

// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "orb.h"

PUSH_WARNINGS
#define STBRP_STATIC                        /* keep all stbrp_* symbols TU-local; gui.lib also exports them */
#define STB_RECT_PACK_IMPLEMENTATION
#include "developer/dev_font/stb_rect_pack.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "developer/dev_font/stb_truetype.h"
POP_WARNINGS

#include "tools/font_tool/orb_font.h"
#include "engine/sys/sys_host.h"
#include "developer/dev_font/dev_font.h"

/*==============================================================================================
    Constants
==============================================================================================*/

#define GLYPH_FIRST   32u
#define GLYPH_LAST    126u
#define GLYPH_MAX     ( GLYPH_LAST - GLYPH_FIRST + 1u )  /* 95 */
#define GLYPH_PAD     1
#define ATLAS_W       512
#define ATLAS_H       512
#define DEV_PATH_MAX  512

/*==============================================================================================
    Platform path helpers
==============================================================================================*/

#if OS_WINDOWS
    #define PATH_SEP         "\\"
    static const char s_sys_font_dir[] = "C:\\Windows\\Fonts";
#else
    #define PATH_SEP         "/"
    static const char s_sys_font_dir[] = "/usr/share/fonts/truetype";
#endif

/*==============================================================================================
    Internal types
==============================================================================================*/

/* Rasterized glyph before atlas placement. */

typedef struct
{
    u32  codepoint;
    u8*  bitmap;    /* stbtt_GetCodepointBitmap result; free with stbtt_FreeBitmap */
    int  w;
    int  h;
    int  ox;        /* bearing_x: cursor-to-left-edge offset, pixels */
    int  oy;        /* stbtt y-offset from baseline to bitmap top (negative = above baseline) */
    int  advance;   /* horizontal advance, pixels */

} raw_glyph_t;

/*==============================================================================================
    Static storage (atlas lives in BSS, not the stack)
==============================================================================================*/

static raw_glyph_t  s_raw   [ GLYPH_MAX ];
static stbrp_node   s_nodes [ ATLAS_W ];
static u8           s_atlas [ ATLAS_W * ATLAS_H ];

/*==============================================================================================
    Module state
==============================================================================================*/

static struct
{
    char build_dir      [ DEV_PATH_MAX ];
    char font_source_dir[ DEV_PATH_MAX ];
    char font_cache_dir [ DEV_PATH_MAX ];
    bool initialized;

} g_rt;

static char g_error[ 512 ];

/*==============================================================================================
    Error
==============================================================================================*/

static void
set_error( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( g_error, sizeof( g_error ), fmt, ap );
    va_end( ap );
}

const char*
dev_font_last_error( void )
{
    return g_error;
}

/*==============================================================================================
    Path helpers
==============================================================================================*/

static bool
has_dir_sep( const char* path )
{
    for ( ; *path; ++path )
        if ( *path == '/' || *path == '\\' ) return true;
    return false;
}

/* Derive a C-safe identifier from the filename stem (e.g. "Consola Mono.ttf" -> "Consola_Mono"). */

static void
derive_stem( const char* path, char* out, int out_size )
{
    const char* base = path;
    for ( const char* p = path; *p; ++p )
        if ( *p == '/' || *p == '\\' ) base = p + 1;

    int len = 0;
    for ( const char* p = base; *p && *p != '.' && len < out_size - 1; ++p )
    {
        char c = *p;
        out[ len++ ] = ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' )
                         || ( c >= '0' && c <= '9' ) ) ? c : '_';
    }
    out[ len ] = '\0';
}

/* Build-root auto-detection: exe lives at <build_root>/bin/<exe>, so strip the last component. */

static void
auto_detect_build_dir( char* out, int size )
{
    char exe_dir[ DEV_PATH_MAX ];
    sys_exe_dir( exe_dir, sizeof( exe_dir ) );

    int len = (int)strlen( exe_dir );
    while ( len > 0 && ( exe_dir[ len - 1 ] == '\\' || exe_dir[ len - 1 ] == '/' ) )
        exe_dir[ --len ] = '\0';

    char* slash = strrchr( exe_dir, '\\' );
    if ( !slash ) slash = strrchr( exe_dir, '/' );
    if ( slash ) *slash = '\0';

    snprintf( out, (size_t)size, "%s", exe_dir );
}

/* Resolve a bare filename or full path to an absolute TTF path that exists on disk.
   Search order for bare filenames: assets/font_source/, then OS system font directory. */

static bool
resolve_ttf( const char* ttf_path, char* out, int size )
{
    if ( has_dir_sep( ttf_path ) )
    {
        if ( sys_file_time( ttf_path ) == 0 )
        {
            set_error( "font not found: '%s'", ttf_path );
            return false;
        }
        snprintf( out, (size_t)size, "%s", ttf_path );
        return true;
    }

    /* assets/font_source/ */
    snprintf( out, (size_t)size, "%s" PATH_SEP "%s", g_rt.font_source_dir, ttf_path );
    if ( sys_file_time( out ) > 0 ) return true;

    /* OS system font directory */
    snprintf( out, (size_t)size, "%s" PATH_SEP "%s", s_sys_font_dir, ttf_path );
    if ( sys_file_time( out ) > 0 ) return true;

    set_error( "font '%s' not found in assets/font_source/ or system fonts", ttf_path );
    return false;
}

/*==============================================================================================
    bake_font -- rasterize, pack, and write a .orb_font to out_path.
==============================================================================================*/

static bool
bake_font( const char* ttf_path, int size_px, const char* out_path )
{
    /*------------------------------------------------------------------------------------------
        Read the TTF file into memory.
    ------------------------------------------------------------------------------------------*/

    FILE* f = fopen( ttf_path, "rb" );
    if ( !f )
    {
        set_error( "cannot open font file '%s'", ttf_path );
        return false;
    }

    fseek( f, 0, SEEK_END );
    long file_size = ftell( f );
    fseek( f, 0, SEEK_SET );

    if ( file_size <= 0 )
    {
        fclose( f );
        set_error( "font file '%s' is empty or unreadable", ttf_path );
        return false;
    }

    u8* font_data = (u8*)malloc( (size_t)file_size );
    if ( !font_data )
    {
        fclose( f );
        set_error( "out of memory reading '%s'", ttf_path );
        return false;
    }

    if ( fread( font_data, 1, (size_t)file_size, f ) != (size_t)file_size )
    {
        free( font_data );
        fclose( f );
        set_error( "read error on '%s'", ttf_path );
        return false;
    }
    fclose( f );

    /*------------------------------------------------------------------------------------------
        Initialize stb_truetype and read global metrics.
    ------------------------------------------------------------------------------------------*/

    stbtt_fontinfo font_info;
    if ( !stbtt_InitFont( &font_info, font_data, stbtt_GetFontOffsetForIndex( font_data, 0 ) ) )
    {
        free( font_data );
        set_error( "stbtt_InitFont failed -- invalid or unsupported font '%s'", ttf_path );
        return false;
    }

    float scale = stbtt_ScaleForPixelHeight( &font_info, (float)size_px );

    int stbtt_ascent_u, stbtt_descent_u, stbtt_line_gap_u;
    stbtt_GetFontVMetrics( &font_info, &stbtt_ascent_u, &stbtt_descent_u, &stbtt_line_gap_u );

    int ascent_px   = (int)roundf( (float)stbtt_ascent_u   * scale );
    int descent_px  = (int)roundf( (float)stbtt_descent_u  * scale );
    int line_gap_px = (int)roundf( (float)stbtt_line_gap_u * scale );

    /*------------------------------------------------------------------------------------------
        Pass 1 -- rasterize glyphs.

        stbtt coordinate convention:
            ox  = bearing_x (cursor-to-left-edge, pixels)
            oy  = y-offset from baseline to bitmap top (negative = above baseline)
        orb_font_glyph_t uses the FreeType convention for bearing_y (positive = above
        baseline), so we store -oy.
    ------------------------------------------------------------------------------------------*/

    memset( s_raw, 0, sizeof( s_raw ) );
    u32 raw_count = 0;

    for ( u32 cp = GLYPH_FIRST; cp <= GLYPH_LAST; ++cp )
    {
        raw_glyph_t* r = &s_raw[ raw_count ];
        r->codepoint   = cp;
        r->bitmap      = NULL;
        r->w = r->h = r->ox = r->oy = 0;

        int adv_u, lsb_u;
        stbtt_GetCodepointHMetrics( &font_info, (int)cp, &adv_u, &lsb_u );
        r->advance = (int)roundf( (float)adv_u * scale );

        int w, h, ox, oy;
        u8* bm = stbtt_GetCodepointBitmap( &font_info, 0, scale, (int)cp, &w, &h, &ox, &oy );
        if ( bm && w > 0 && h > 0 )
        {
            r->bitmap = bm;
            r->w = w;  r->h = h;
            r->ox = ox; r->oy = oy;
        }
        else if ( bm )
        {
            stbtt_FreeBitmap( bm, NULL );
        }

        ++raw_count;
    }

    free( font_data );

    /*------------------------------------------------------------------------------------------
        Pass 2 -- pack glyph rects.
        GLYPH_PAD adds a 1-pixel gap between neighbours to prevent bilinear filter bleed.
    ------------------------------------------------------------------------------------------*/

    stbrp_context pack_ctx;
    stbrp_init_target( &pack_ctx, ATLAS_W, ATLAS_H, s_nodes, ATLAS_W );

    stbrp_rect rects[ GLYPH_MAX ];
    int        rect_count = 0;

    for ( u32 i = 0; i < raw_count; ++i )
    {
        if ( !s_raw[ i ].bitmap ) continue;
        rects[ rect_count ].id         = (int)i;
        rects[ rect_count ].w          = (stbrp_coord)( s_raw[ i ].w + GLYPH_PAD );
        rects[ rect_count ].h          = (stbrp_coord)( s_raw[ i ].h + GLYPH_PAD );
        rects[ rect_count ].was_packed = 0;
        ++rect_count;
    }

    if ( rect_count > 0 && !stbrp_pack_rects( &pack_ctx, rects, rect_count ) )
    {
        for ( u32 i = 0; i < raw_count; ++i )
            if ( s_raw[ i ].bitmap ) stbtt_FreeBitmap( s_raw[ i ].bitmap, NULL );
        set_error( "atlas %dx%d too small for %u glyphs at %d px -- try a smaller size",
                   ATLAS_W, ATLAS_H, raw_count, size_px );
        return false;
    }

    /*------------------------------------------------------------------------------------------
        Pass 3 -- blit bitmaps into atlas; build glyph records.
    ------------------------------------------------------------------------------------------*/

    memset( s_atlas, 0, sizeof( s_atlas ) );

    orb_font_glyph_t out_glyphs[ GLYPH_MAX ];
    memset( out_glyphs, 0, sizeof( out_glyphs ) );

    /* Pre-fill every record with codepoint and advance; non-bitmapped glyphs (whitespace) keep
       zero atlas coords and zero dimensions, which the renderer treats as invisible. */
    for ( u32 i = 0; i < raw_count; ++i )
    {
        out_glyphs[ i ].codepoint = s_raw[ i ].codepoint;
        out_glyphs[ i ].advance   = (u16)s_raw[ i ].advance;
    }

    for ( int ri = 0; ri < rect_count; ++ri )
    {
        if ( !rects[ ri ].was_packed ) continue;

        raw_glyph_t*      r  = &s_raw[ rects[ ri ].id ];
        orb_font_glyph_t* og = &out_glyphs[ rects[ ri ].id ];

        og->atlas_x   = (u16)rects[ ri ].x;
        og->atlas_y   = (u16)rects[ ri ].y;
        og->w         = (u16)r->w;
        og->h         = (u16)r->h;
        og->bearing_x = (i16)r->ox;
        og->bearing_y = (i16)( -r->oy );   /* FT convention: positive = above baseline */

        for ( int row = 0; row < r->h; ++row )
        {
            const u8* src = r->bitmap + row * r->w;
            u8*       dst = s_atlas + ( (u32)rects[ ri ].y + (u32)row ) * ATLAS_W
                                    + (u32)rects[ ri ].x;
            memcpy( dst, src, (size_t)r->w );
        }
    }

    for ( u32 i = 0; i < raw_count; ++i )
    {
        if ( s_raw[ i ].bitmap )
        {
            stbtt_FreeBitmap( s_raw[ i ].bitmap, NULL );
            s_raw[ i ].bitmap = NULL;
        }
    }

    /*------------------------------------------------------------------------------------------
        Write .orb_font.
    ------------------------------------------------------------------------------------------*/

    FILE* out = fopen( out_path, "wb" );
    if ( !out )
    {
        set_error( "cannot write cache file '%s'", out_path );
        return false;
    }

    orb_font_header_t hdr;
    memset( &hdr, 0, sizeof( hdr ) );
    hdr.magic       = ORB_FONT_MAGIC;
    hdr.version     = ORB_FONT_VERSION;
    hdr.atlas_w     = ATLAS_W;
    hdr.atlas_h     = ATLAS_H;
    hdr.font_size   = (u32)size_px;
    hdr.ascent      = ascent_px;
    hdr.descent     = descent_px;
    hdr.line_gap    = line_gap_px;
    hdr.glyph_count = raw_count;

    fwrite( &hdr,       sizeof( hdr ),              1,         out );
    fwrite( out_glyphs, sizeof( orb_font_glyph_t ), raw_count, out );
    fwrite( s_atlas,    1,                           ATLAS_W * ATLAS_H, out );
    fclose( out );

    printf( "[dev_font] baked '%s' %d px -> '%s' (%u glyphs, ascent %d, descent %d)\n",
            ttf_path, size_px, out_path, raw_count, ascent_px, descent_px );
    return true;
}

/*==============================================================================================
    Public
==============================================================================================*/

bool
dev_font_init( const dev_font_settings_t* settings )
{
    memset( &g_rt, 0, sizeof( g_rt ) );

    if ( settings && settings->build_dir && *settings->build_dir )
        snprintf( g_rt.build_dir, sizeof( g_rt.build_dir ), "%s", settings->build_dir );
    else
        auto_detect_build_dir( g_rt.build_dir, sizeof( g_rt.build_dir ) );

    snprintf( g_rt.font_source_dir, sizeof( g_rt.font_source_dir ),
              "%s" PATH_SEP "assets" PATH_SEP "font_source", g_rt.build_dir );
    snprintf( g_rt.font_cache_dir, sizeof( g_rt.font_cache_dir ),
              "%s" PATH_SEP "assets" PATH_SEP "font_cache", g_rt.build_dir );

    g_rt.initialized = true;

    printf( "[dev_font] init  build=%s  source=%s  cache=%s\n",
            g_rt.build_dir, g_rt.font_source_dir, g_rt.font_cache_dir );
    return true;
}

void
dev_font_shutdown( void )
{
    memset( &g_rt, 0, sizeof( g_rt ) );
}

bool
dev_font_get( const char* ttf_path, int size_px, char* out_path, int out_path_size )
{
    if ( !g_rt.initialized )
    {
        set_error( "dev_font_init() not called" );
        return false;
    }
    if ( !ttf_path || !*ttf_path )
    {
        set_error( "ttf_path is required" );
        return false;
    }
    if ( size_px < 6 || size_px > 256 )
    {
        set_error( "size_px must be 6..256, got %d", size_px );
        return false;
    }
    if ( !out_path || out_path_size < 2 )
    {
        set_error( "out_path buffer is NULL or too small" );
        return false;
    }

    /* Resolve the source TTF to an absolute path that exists on disk. */

    char ttf_abs[ DEV_PATH_MAX ];
    if ( !resolve_ttf( ttf_path, ttf_abs, sizeof( ttf_abs ) ) )
        return false;

    /* Derive cache filename: assets/font_cache/<stem>_<size>px.orb_font */

    char stem[ 64 ];
    derive_stem( ttf_abs, stem, sizeof( stem ) );

    char cache_path[ DEV_PATH_MAX ];
    snprintf( cache_path, sizeof( cache_path ), "%s" PATH_SEP "%s_%dpx.orb_font",
              g_rt.font_cache_dir, stem, size_px );

    /* Cache hit: skip baking when the cached file is at least as new as the source TTF. */

    u64 cache_time = sys_file_time( cache_path );
    u64 ttf_time   = sys_file_time( ttf_abs );
    if ( cache_time > 0 && ttf_time > 0 && cache_time >= ttf_time )
    {
        snprintf( out_path, (size_t)out_path_size, "%s", cache_path );
        return true;
    }

    if ( !bake_font( ttf_abs, size_px, cache_path ) )
        return false;

    snprintf( out_path, (size_t)out_path_size, "%s", cache_path );
    return true;
}

// clang-format on
/*============================================================================================*/
