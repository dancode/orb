/*==============================================================================================

    dev_font.c -- Developer runtime font baker.

    Unity build entry for the dev_font static library.  Defines STB_RECT_PACK_IMPLEMENTATION
    and STB_TRUETYPE_IMPLEMENTATION here so no other TU pulls them in.

    Pipeline (identical output format to font_tool.exe):
        Pass 1 -- rasterize glyphs with stb_truetype into heap bitmaps.
        Pass 2 -- pack glyph rects with stb_rect_pack (GLYPH_PAD gap between each), trying
                  progressively larger power-of-two squares (ATLAS_MIN..ATLAS_MAX) and stopping
                  at the first that fits, so small fonts don't waste a fixed 512x512 canvas.
        Pass 3 -- blit bitmaps into the chosen-size R8 atlas; write orb_font_header_t + glyph
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

/* Baked codepoint range comes from the format contract (orb_font.h): ORB_FONT_CP_FIRST/LAST/COUNT
   is the ASCII default (this file's own stb baker bakes exactly that); ORB_FONT_MAX_GLYPHS caps
   what the shared back-end accepts, since font_tool -range feeds extended sets through it. */
#define GLYPH_PAD     1
#define ATLAS_MIN     64        // smallest atlas size attempted (px, square)
#define ATLAS_MAX     1024      // largest atlas size attempted (px, square); also static buffer cap
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
    Static storage (atlas lives in BSS, not the stack)
==============================================================================================*/

static stbrp_node       s_nodes  [ ATLAS_MAX ];
/* Square atlas, at most ATLAS_MAX per side (glyphs are packed full-height; no reserved band).
   1 MiB of BSS at 1024 -- dev-only tooling, and heap-free by the library's charter. */
static u8               s_atlas  [ ATLAS_MAX * ATLAS_MAX ];
/* Rasterized-glyph scratch for THIS file's stb baker, which bakes the ASCII contract only. */
static dev_font_glyph_t s_glyphs [ ORB_FONT_CP_COUNT ];
/* Pack scratch + output records for dev_font_bake_write -- sized to the format cap, not the
   ASCII count, because font_tool -range feeds extended sets through the shared back-end. */
static stbrp_rect       s_rects      [ ORB_FONT_MAX_GLYPHS ];
static orb_font_glyph_t s_out_glyphs [ ORB_FONT_MAX_GLYPHS ];

/*==============================================================================================
    Module state
==============================================================================================*/

static struct
{
    char build_dir      [ DEV_PATH_MAX ];
    char font_source_dir[ DEV_PATH_MAX ];   // assets/font_source -- source .ttf inputs
    char font_cache_dir [ DEV_PATH_MAX ];   // assets/font_cache  -- quick stb bakes (dev_font_get)
    char font_dir       [ DEV_PATH_MAX ];   // assets/font        -- final orb bakes (font_tool)
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

/* Lowercase, alphanumeric-only copy of `s`.  Lets a requested font name match filenames and OS
   registry entries regardless of spaces, case, or punctuation ("Cascadia Mono" -> "cascadiamono"
   == "CascadiaMono.ttf"). */

static void
normalize_name( const char* s, char* out, int out_size )
{
    int len = 0;
    for ( const char* p = s; *p && len < out_size - 1; ++p )
    {
        char c = *p;
        if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
        if ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) )
            out[ len++ ] = c;
    }
    out[ len ] = '\0';
}

/* Normalized stem of a path's filename (directory and everything from the first '.' dropped). */

static void
normalize_stem( const char* path, char* out, int out_size )
{
    char raw[ 128 ];
    derive_stem( path, raw, sizeof( raw ) );
    normalize_name( raw, out, out_size );
}

/*==============================================================================================
    By-name resolution -- match a friendly font name to a file on disk.
==============================================================================================*/

/* Context threaded through the glob callback while scanning a directory for a name match. */

typedef struct
{
    const char* want_norm;   /* normalized target name */
    char*       out;
    int         out_size;
    bool        found;

} scan_ctx_t;

static bool
scan_match_cb( const char* filename, const char* full_path, void* ud )
{
    scan_ctx_t* c = (scan_ctx_t*)ud;
    char        norm[ 128 ];
    normalize_stem( filename, norm, sizeof( norm ) );
    if ( strcmp( norm, c->want_norm ) == 0 )
    {
        snprintf( c->out, (size_t)c->out_size, "%s", full_path );
        c->found = true;
        return false;   /* stop iterating */
    }
    return true;
}

/* Scan `dir` for a TTF/OTF/TTC whose normalized stem equals `want_norm`.  Portable fallback that
   matches names differing from the filename only by spaces/case ("Cascadia Mono" -> CascadiaMono). */

static bool
scan_dir_for_name( const char* dir, const char* want_norm, char* out, int out_size )
{
    static const char* pats[] = { "*.ttf", "*.otf", "*.ttc" };
    scan_ctx_t         c       = { want_norm, out, out_size, false };
    for ( int i = 0; i < 3 && !c.found; ++i )
        sys_file_glob( dir, pats[ i ], scan_match_cb, &c );
    return c.found;
}

/* Resolve a bare filename, a friendly font name, or a full path to an absolute TTF path that
   exists on disk.  For bare requests the search order is:
       1. assets/font_source/    -- exact name, then + .ttf / .otf / .ttc
       2. OS system font dir      -- exact filename, then + .ttf / .otf / .ttc
       3. by friendly name        -- OS font registry (Windows), then a normalized-stem scan of
                                     font_source/ and the system font dir (portable)
   A request that already contains a separator is used as-is. */

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

    /* "" leaves the request untouched (it may already carry an extension); the rest probe the
       common face extensions so a bare family name resolves to a file. */
    static const char* ext[] = { "", ".ttf", ".otf", ".ttc" };

    /* 1. assets/font_source/ */
    for ( int i = 0; i < 4; ++i )
    {
        snprintf( out, (size_t)size, "%s" PATH_SEP "%s%s", g_rt.font_source_dir, ttf_path, ext[ i ] );
        if ( sys_file_time( out ) > 0 ) return true;
    }

    /* 2. OS system font directory */
    for ( int i = 0; i < 4; ++i )
    {
        snprintf( out, (size_t)size, "%s" PATH_SEP "%s%s", s_sys_font_dir, ttf_path, ext[ i ] );
        if ( sys_file_time( out ) > 0 ) return true;
    }

    /* 3. By friendly name (handles files named differently from the font, e.g. Consolas ->
       consola.ttf, or Cascadia Mono -> CascadiaMono.ttf).  The OS font registry is authoritative;
       a normalized-stem scan of our own dirs is the portable fallback (and the POSIX path). */
    if ( sys_font_resolve_name( ttf_path, out, size ) ) return true;

    char want[ 128 ];
    normalize_name( ttf_path, want, sizeof( want ) );
    if ( scan_dir_for_name( g_rt.font_source_dir, want, out, size ) ) return true;
    if ( scan_dir_for_name( s_sys_font_dir,       want, out, size ) ) return true;

    set_error( "font '%s' not found in assets/font_source/, system fonts, or by name", ttf_path );
    return false;
}

/*==============================================================================================
    dev_font_bake_write -- shared bake back-end (pack + blit + write), used by both bakers.

    The front-end (stb_truetype here, FreeType in font_tool) rasterizes glyphs into a
    dev_font_glyph_t[] and hands them here; this owns the packing heuristic and the .orb_font file
    layout so neither is duplicated.  The caller retains ownership of each glyph bitmap.
==============================================================================================*/

bool
dev_font_bake_write( const char* out_path, const dev_font_glyph_t* glyphs, u32 count,
                     int ascent, int descent, int line_gap, int size_px,
                     u32 sdf_range, const char* label )
{
    if ( count > ORB_FONT_MAX_GLYPHS )
    {
        set_error( "glyph count %u exceeds format cap %u", count, (u32)ORB_FONT_MAX_GLYPHS );
        return false;
    }

    /*------------------------------------------------------------------------------------------
        Pass 2 -- pack glyph rects into the smallest square atlas (ATLAS_MIN..ATLAS_MAX per side)
        that fits every glyph.  A fixed 512x512 canvas wastes most of its area for small fonts, so
        try progressively larger square sizes and stop at the first that works.  Bitmap-less glyphs
        (whitespace) are skipped -- they carry no coverage to place.
        GLYPH_PAD adds a 1-pixel gap between neighbours to prevent bilinear filter bleed.  The atlas
        is pure glyph coverage -- gui draws its white texel + dash rows from a shared runtime atlas,
        so no reserved band is packed here.
    ------------------------------------------------------------------------------------------*/

    stbrp_rect* rects      = s_rects;
    int         rect_count = 0;

    for ( u32 i = 0; i < count; ++i )
    {
        if ( !glyphs[ i ].bitmap ) continue;
        rects[ rect_count ].id = (int)i;
        rects[ rect_count ].w  = (stbrp_coord)( glyphs[ i ].w + GLYPH_PAD );
        rects[ rect_count ].h  = (stbrp_coord)( glyphs[ i ].h + GLYPH_PAD );
        ++rect_count;
    }

    stbrp_context pack_ctx;
    u32           atlas_w = ATLAS_MIN, atlas_h = ATLAS_MIN;

    if ( rect_count > 0 )
    {
        atlas_w = atlas_h = 0;
        for ( u32 try_size = ATLAS_MIN; try_size <= ATLAS_MAX; try_size *= 2 )
        {
            for ( int i = 0; i < rect_count; ++i )
                rects[ i ].was_packed = 0;

            stbrp_init_target( &pack_ctx, (int)try_size, (int)try_size, s_nodes, (int)try_size );
            /* BL (the default heuristic) does not check the height bound while searching for a
               placement -- only BF does (see stb_rect_pack.h stbrp__skyline_find_best_pos).
               Without this, a "successful" pack can silently place a rect below the requested
               height. */
            stbrp_setup_heuristic( &pack_ctx, STBRP_HEURISTIC_Skyline_BF_sortHeight );
            if ( stbrp_pack_rects( &pack_ctx, rects, rect_count ) )
            {
                atlas_w = atlas_h = try_size;
                break;
            }
        }

        if ( atlas_w == 0 )
        {
            set_error( "%ux%u atlas too small for %u glyphs at %d px -- try a smaller size",
                       ATLAS_MAX, ATLAS_MAX, count, size_px );
            return false;
        }
    }

    /*------------------------------------------------------------------------------------------
        Pass 3 -- blit bitmaps into atlas; build glyph records.
    ------------------------------------------------------------------------------------------*/

    memset( s_atlas, 0, (size_t)atlas_w * atlas_h );

    u32 packed_area = 0;

    orb_font_glyph_t* out_glyphs = s_out_glyphs;
    memset( out_glyphs, 0, (size_t)count * sizeof( orb_font_glyph_t ) );

    /* Pre-fill every record with codepoint and advance; non-bitmapped glyphs (whitespace) keep
       zero atlas coords and zero dimensions, which the renderer treats as invisible. */
    for ( u32 i = 0; i < count; ++i )
    {
        out_glyphs[ i ].codepoint = glyphs[ i ].codepoint;
        out_glyphs[ i ].advance   = (u16)glyphs[ i ].advance;
    }

    u32 used_h = 0;   /* bottom-most packed row + 1 -- the page is cropped to this (see below) */

    for ( int ri = 0; ri < rect_count; ++ri )
    {
        if ( !rects[ ri ].was_packed ) continue;

        const dev_font_glyph_t* r  = &glyphs[ rects[ ri ].id ];
        orb_font_glyph_t*       og = &out_glyphs[ rects[ ri ].id ];

        if ( (u32)rects[ ri ].y + (u32)r->h > used_h )
            used_h = (u32)rects[ ri ].y + (u32)r->h;

        og->atlas_x   = (u16)rects[ ri ].x;
        og->atlas_y   = (u16)rects[ ri ].y;
        og->w         = (u16)r->w;
        og->h         = (u16)r->h;
        og->bearing_x = (i16)r->bearing_x;
        og->bearing_y = (i16)r->bearing_y;

        packed_area += (u32)( r->w * r->h );

        for ( int row = 0; row < r->h; ++row )
        {
            const u8* src = r->bitmap + row * r->w;
            u8*       dst = s_atlas + ( (u32)rects[ ri ].y + (u32)row ) * atlas_w
                                    + (u32)rects[ ri ].x;
            memcpy( dst, src, (size_t)r->w );
        }
    }

    /*------------------------------------------------------------------------------------------
        Crop the page to the rows actually packed.

        The size search above only tries SQUARE powers of two, so a glyph set that overflows one
        step lands in the next and leaves most of it empty -- a 16px SDF face fills 512 wide but
        only ~153 rows of a 512x512 page, so 70% of the file, the upload, and the runtime atlas
        tenant is blank.  Height is the free axis: the buffer is row-major at a fixed atlas_w
        stride, so the packed rows are already contiguous from the top and cropping is a smaller
        write, not a re-blit.  Width is not croppable the same way and is not worth it -- the
        skyline fills the full width first, which is exactly why the waste is all vertical.

        No trailing gutter row is kept.  Within the page GLYPH_PAD separates NEIGHBOURS, and below
        the last row there is no neighbour; at the runtime atlas the page is a tenant with its own
        ring, which for a LINEAR-sampled atlas is edge-replicated (gui_res_atlas.c, extrude).
    ------------------------------------------------------------------------------------------*/

    if ( used_h > 0 && used_h < atlas_h )
        atlas_h = used_h;

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
    hdr.atlas_w     = atlas_w;
    hdr.atlas_h     = atlas_h;
    hdr.font_size   = (u32)size_px;
    hdr.ascent      = ascent;
    hdr.descent     = descent;
    hdr.line_gap    = line_gap;
    hdr.glyph_count = count;
    hdr.sdf_range   = sdf_range;

    fwrite( &hdr,       sizeof( hdr ),              1,         out );
    fwrite( out_glyphs, sizeof( orb_font_glyph_t ), count,     out );
    fwrite( s_atlas,    1,                           (size_t)atlas_w * atlas_h, out );
    fclose( out );

    f32 usage_pct = 100.0f * (f32)packed_area / ( (f32)atlas_w * (f32)atlas_h );
    printf( "[dev_font] baked '%s' %d px -> '%s' (%u glyphs, %ux%u atlas, %.1f%% used, ascent %d, descent %d%s",
            label, size_px, out_path, count, atlas_w, atlas_h, usage_pct, ascent, descent,
            sdf_range ? ", " : ")\n" );
    if ( sdf_range )
        printf( "sdf spread %u px)\n", sdf_range );
    return true;
}

/*==============================================================================================
    bake_font -- stb_truetype front-end: rasterize glyphs, then hand off to dev_font_bake_write.
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

    float scale = stbtt_ScaleForMappingEmToPixels( &font_info, (float)size_px );

    int stbtt_ascent_u, stbtt_descent_u, stbtt_line_gap_u;
    stbtt_GetFontVMetrics( &font_info, &stbtt_ascent_u, &stbtt_descent_u, &stbtt_line_gap_u );

    int ascent_px   = (int)roundf( (float)stbtt_ascent_u   * scale );
    int descent_px  = (int)roundf( (float)stbtt_descent_u  * scale );
    int line_gap_px = (int)roundf( (float)stbtt_line_gap_u * scale );

    /*------------------------------------------------------------------------------------------
        Pass 1 -- rasterize glyphs into the shared dev_font_glyph_t scratch.

        stbtt coordinate convention:
            ox  = bearing_x (cursor-to-left-edge, pixels)
            oy  = y-offset from baseline to bitmap top (negative = above baseline)
        dev_font_glyph_t uses the orb_font/FreeType convention for bearing_y (positive = above
        baseline), so we store -oy.
    ------------------------------------------------------------------------------------------*/

    memset( s_glyphs, 0, sizeof( s_glyphs ) );
    u32 raw_count = 0;

    for ( u32 cp = ORB_FONT_CP_FIRST; cp <= ORB_FONT_CP_LAST; ++cp )
    {
        dev_font_glyph_t* r = &s_glyphs[ raw_count++ ];
        r->codepoint        = cp;

        int adv_u, lsb_u;
        stbtt_GetCodepointHMetrics( &font_info, (int)cp, &adv_u, &lsb_u );
        r->advance = (int)roundf( (float)adv_u * scale );

        int w, h, ox, oy;
        u8* bm = stbtt_GetCodepointBitmap( &font_info, 0, scale, (int)cp, &w, &h, &ox, &oy );
        if ( bm && w > 0 && h > 0 )
        {
            r->bitmap    = bm;
            r->w         = w;
            r->h         = h;
            r->bearing_x = ox;
            r->bearing_y = -oy;   /* orb_font convention: positive = above baseline */
        }
        else if ( bm )
        {
            stbtt_FreeBitmap( bm, NULL );
        }
    }

    free( font_data );

    /* Pass 2 + 3 + write live in the shared back-end. */
    /* Always coverage: stb_truetype rasterizes alpha, and the SDF path is FreeType-only (font_tool).
       A dev-cache bake is a quick stand-in for a shipped atlas, and an SDF font is a deliberate
       authored choice, not something to fall into by editing a TTF. */
    bool ok = dev_font_bake_write( out_path, s_glyphs, raw_count,
                                   ascent_px, descent_px, line_gap_px, size_px, 0u, ttf_path );

    for ( u32 i = 0; i < raw_count; ++i )
        if ( s_glyphs[ i ].bitmap )
        {
            stbtt_FreeBitmap( s_glyphs[ i ].bitmap, NULL );
            s_glyphs[ i ].bitmap = NULL;
        }

    return ok;
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
    snprintf( g_rt.font_dir, sizeof( g_rt.font_dir ),
              "%s" PATH_SEP "assets" PATH_SEP "font", g_rt.build_dir );

    g_rt.initialized = true;

    printf( "[dev_font] init  build=%s  source=%s  cache=%s  font=%s\n",
            g_rt.build_dir, g_rt.font_source_dir, g_rt.font_cache_dir, g_rt.font_dir );
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

    sys_dir_make( g_rt.font_cache_dir );   // create assets/font_cache/ on first bake

    if ( !bake_font( ttf_abs, size_px, cache_path ) )
        return false;

    snprintf( out_path, (size_t)out_path_size, "%s", cache_path );
    return true;
}

bool
dev_font_resolve( const char* request, char* out_path, int out_path_size )
{
    if ( !g_rt.initialized )
    {
        set_error( "dev_font_init() not called" );
        return false;
    }
    if ( !request || !*request )
    {
        set_error( "request is required" );
        return false;
    }
    if ( !out_path || out_path_size < 2 )
    {
        set_error( "out_path buffer is NULL or too small" );
        return false;
    }
    return resolve_ttf( request, out_path, out_path_size );
}

bool
dev_font_source_dir( char* out_path, int out_path_size )
{
    if ( !g_rt.initialized ) return false;
    snprintf( out_path, (size_t)out_path_size, "%s", g_rt.font_source_dir );
    return true;
}

bool
dev_font_dir( char* out_path, int out_path_size )
{
    if ( !g_rt.initialized ) return false;
    snprintf( out_path, (size_t)out_path_size, "%s", g_rt.font_dir );
    return true;
}

// clang-format on
/*============================================================================================*/
