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
#define GLYPH_PAD        1
#define ATLAS_MIN        64                          // smallest page attempted (px, square)
#define ATLAS_NODE_MAX   ORB_FONT_PAGE_MAX_W_SDF     // widest pack target -> stbrp node count
#define ATLAS_PIXEL_MAX  ( 1024u * 1024u )           // page buffer capacity (1 MiB BSS)
#define DEV_PATH_MAX     512

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

static stbrp_node       s_nodes  [ ATLAS_NODE_MAX ];
/* Page buffer -- glyphs are packed full-height, no reserved band; dev-only tooling, and
   heap-free by the library's charter.  Every ladder candidate below asserts it fits this. */
static u8               s_atlas  [ ATLAS_PIXEL_MAX ];
/* Rasterized-glyph scratch for THIS file's stb baker -- sized to the format cap because a
   range spec can feed extended sets through it (dev_font_get_ex's range_spec). */
static dev_font_glyph_t s_glyphs [ ORB_FONT_MAX_GLYPHS ];
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
        /* Absolute (or already-joined) path: use as-is. */
        if ( sys_file_time( ttf_path ) > 0 )
        {
            snprintf( out, (size_t)size, "%s", ttf_path );
            return true;
        }

        /* Otherwise treat it as font_source/-relative, e.g. "Roboto/Roboto-Bold.ttf" for a face
           organized into a subdirectory. */
        snprintf( out, (size_t)size, "%s" PATH_SEP "%s", g_rt.font_source_dir, ttf_path );
        if ( sys_file_time( out ) > 0 ) return true;

        set_error( "font not found: '%s'", ttf_path );
        return false;
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
    Codepoint ranges -- what a -range spec parses into.

    NULL / empty means exactly the ASCII contract from orb_font.h, so a default bake stays
    byte-identical to every bake made before ranges existed.  A spec is comma-separated tokens,
    each a preset name or an explicit LO[-HI] span (hex or decimal):
        "latin1"        "latin,greek,cyrillic"        "ascii,0x2022"
    Spans are sorted and merged before baking, so overlapping presets never bake a glyph twice.
==============================================================================================*/

typedef struct
{
    const char*      name;
    dev_font_range_t span[ 2 ];
    int              count;

} range_preset_t;

/* Presets are PRINTABLE spans; a codepoint the face has no glyph for is skipped at bake, so a
   generous span costs nothing in the file. */
static const range_preset_t s_presets[] = {
    { "ascii",    { { 0x0020, 0x007E } },                     1 },   /* the default            */
    { "latin1",   { { 0x0020, 0x007E }, { 0x00A0, 0x00FF } }, 2 },   /* + Latin-1 Supplement   */
    { "latin",    { { 0x0020, 0x007E }, { 0x00A0, 0x017F } }, 2 },   /* + Latin Extended-A     */
    { "greek",    { { 0x0370, 0x03FF } },                     1 },   /* Greek and Coptic       */
    { "cyrillic", { { 0x0400, 0x04FF } },                     1 },   /* Cyrillic               */
};

static bool
range_add( dev_font_range_t* out, int cap, int* count, u32 lo, u32 hi )
{
    if ( *count == cap )
    {
        set_error( "range spec has too many spans (max %d)", cap );
        return false;
    }
    out[ *count ].lo = lo;
    out[ *count ].hi = hi;
    ++*count;
    return true;
}

/* One spec token: a preset name, a single codepoint, or "LO-HI" (strtoul base 0: hex or decimal). */

static bool
range_parse_token( const char* tok, size_t len, dev_font_range_t* out, int cap, int* count )
{
    char          buf[ 32 ];
    char*         end;
    unsigned long lo, hi;

    for ( size_t i = 0; i < sizeof( s_presets ) / sizeof( s_presets[ 0 ] ); ++i )
    {
        const range_preset_t* p = &s_presets[ i ];
        if ( strlen( p->name ) == len && strncmp( tok, p->name, len ) == 0 )
        {
            for ( int s = 0; s < p->count; ++s )
                if ( !range_add( out, cap, count, p->span[ s ].lo, p->span[ s ].hi ) )
                    return false;
            return true;
        }
    }

    if ( len == 0 || len >= sizeof( buf ) )
        goto bad;
    memcpy( buf, tok, len );
    buf[ len ] = '\0';

    lo = strtoul( buf, &end, 0 );
    hi = lo;
    if ( end == buf )
        goto bad;
    if ( *end == '-' )
    {
        const char* hs = end + 1;
        hi = strtoul( hs, &end, 0 );
        if ( end == hs )
            goto bad;
    }
    if ( *end != '\0' || lo < 0x20 || hi < lo || hi > 0x10FFFF )
        goto bad;
    return range_add( out, cap, count, (u32)lo, (u32)hi );

bad:
    set_error( "bad range token '%.*s' (preset name or LO[-HI], e.g. latin1 or 0xA0-0xFF)",
               (int)len, tok );
    return false;
}

int
dev_font_range_parse( const char* spec, dev_font_range_t* out, int cap )
{
    int count = 0;

    if ( !out || cap < 1 )
    {
        set_error( "range output buffer is NULL or empty" );
        return 0;
    }

    if ( !spec || !*spec )   /* the ASCII contract */
    {
        out[ 0 ].lo = ORB_FONT_CP_FIRST;
        out[ 0 ].hi = ORB_FONT_CP_LAST;
        return 1;
    }

    const char* p = spec;
    while ( *p )
    {
        const char* q = p;
        while ( *q && *q != ',' )
            ++q;
        if ( !range_parse_token( p, (size_t)( q - p ), out, cap, &count ) )
            return 0;
        p = ( *q == ',' ) ? q + 1 : q;
    }
    if ( count == 0 )
    {
        set_error( "range spec is empty" );
        return 0;
    }

    /* Sort by lo and merge touching spans, so overlapping tokens never bake a codepoint twice. */
    for ( int i = 1; i < count; ++i )     /* insertion sort -- the list is tiny */
    {
        dev_font_range_t r = out[ i ];
        int              j = i;
        while ( j > 0 && out[ j - 1 ].lo > r.lo )
        {
            out[ j ] = out[ j - 1 ];
            --j;
        }
        out[ j ] = r;
    }
    int w = 0;
    for ( int i = 1; i < count; ++i )
    {
        if ( out[ i ].lo <= out[ w ].hi + 1u )
        {
            if ( out[ i ].hi > out[ w ].hi )
                out[ w ].hi = out[ i ].hi;
        }
        else
        {
            out[ ++w ] = out[ i ];
        }
    }
    return w + 1;
}

void
dev_font_range_suffix( const char* spec, char* out, int out_size )
{
    if ( !out || out_size < 1 )
        return;
    out[ 0 ] = '\0';
    if ( !spec || !*spec || out_size < 3 )
        return;

    int n      = 0;
    out[ n++ ] = '_';
    for ( const char* p = spec; *p && n < out_size - 1; ++p )
    {
        char c = *p;
        if ( c >= 'A' && c <= 'Z' )
            c = (char)( c - 'A' + 'a' );
        out[ n++ ] = ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) ? c : '-';
    }
    out[ n ] = '\0';
}

static u32
range_codepoint_count( const dev_font_range_t* ranges, int count )
{
    u32 n = 0;
    for ( int i = 0; i < count; ++i )
        n += ranges[ i ].hi - ranges[ i ].lo + 1u;
    return n;
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
        is pure glyph coverage -- gui draws its dash rows from a shared runtime atlas, so no
        reserved band is packed here.
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

    /* Candidate page sizes per DESTINATION, tried in order, first fit wins.  Squares first --
       every bake that fit one before keeps its exact output -- then talls pinned at the
       destination atlas's width cap (orb_font.h).  Wider is impossible: the page becomes ONE
       tenant of that atlas and its width cap is a hard placement bound.  Wider is also what to
       WANT within the bound: crop height ~= glyph area / width, so the widest legal page is the
       shortest tenant, and height is the axis the runtime atlas actually runs out of.
       Coverage has no 512 square -- a 512-wide page + 1px pad already misses the 512-wide
       coverage atlas, so that rung could only produce unloadable files.  The SDF ladder keeps it
       (512 + 2 fits the 1024-wide SDF atlas), preserving existing SDF bakes byte for byte. */
    typedef struct { u32 w, h; } atlas_try_t;
    static const atlas_try_t s_tries_coverage[] = {
        { 64, 64 }, { 128, 128 }, { 256, 256 },
        { ORB_FONT_PAGE_MAX_W_COVERAGE, 512 },
        { ORB_FONT_PAGE_MAX_W_COVERAGE, 1024 },
        { ORB_FONT_PAGE_MAX_W_COVERAGE, 2048 },
    };
    static const atlas_try_t s_tries_sdf[] = {
        { 64, 64 }, { 128, 128 }, { 256, 256 }, { 512, 512 },
        { ORB_FONT_PAGE_MAX_W_SDF, 512 },
        { ORB_FONT_PAGE_MAX_W_SDF, 1024 },
    };
    _Static_assert( ORB_FONT_PAGE_MAX_W_COVERAGE * 2048u <= ATLAS_PIXEL_MAX,
                    "coverage ladder exceeds the page buffer" );
    _Static_assert( ORB_FONT_PAGE_MAX_W_SDF * 1024u <= ATLAS_PIXEL_MAX,
                    "sdf ladder exceeds the page buffer" );

    const atlas_try_t* tries     = sdf_range ? s_tries_sdf : s_tries_coverage;
    u32                try_count = sdf_range
                                 ? (u32)( sizeof( s_tries_sdf )      / sizeof( atlas_try_t ) )
                                 : (u32)( sizeof( s_tries_coverage ) / sizeof( atlas_try_t ) );

    stbrp_context pack_ctx;
    u32           atlas_w = ATLAS_MIN, atlas_h = ATLAS_MIN;

    if ( rect_count > 0 )
    {
        atlas_w = atlas_h = 0;
        for ( u32 t = 0; t < try_count; ++t )
        {
            for ( int i = 0; i < rect_count; ++i )
                rects[ i ].was_packed = 0;

            stbrp_init_target( &pack_ctx, (int)tries[ t ].w, (int)tries[ t ].h,
                               s_nodes, (int)tries[ t ].w );
            /* BL (the default heuristic) does not check the height bound while searching for a
               placement -- only BF does (see stb_rect_pack.h stbrp__skyline_find_best_pos).
               Without this, a "successful" pack can silently place a rect below the requested
               height. */
            stbrp_setup_heuristic( &pack_ctx, STBRP_HEURISTIC_Skyline_BF_sortHeight );
            if ( stbrp_pack_rects( &pack_ctx, rects, rect_count ) )
            {
                atlas_w = tries[ t ].w;
                atlas_h = tries[ t ].h;
                break;
            }
        }

        if ( atlas_w == 0 )
        {
            set_error( "%ux%u atlas too small for %u glyphs at %d px -- try a smaller size or range",
                       tries[ try_count - 1u ].w, tries[ try_count - 1u ].h, count, size_px );
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
bake_font( const char* ttf_path, int size_px,
           const dev_font_range_t* ranges, int range_count, const char* out_path )
{
    if ( range_codepoint_count( ranges, range_count ) > ORB_FONT_MAX_GLYPHS )
    {
        set_error( "range spans %u codepoints, format cap is %u",
                   range_codepoint_count( ranges, range_count ), (u32)ORB_FONT_MAX_GLYPHS );
        return false;
    }

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

    for ( int ri = 0; ri < range_count; ++ri )
    for ( u32 cp = ranges[ ri ].lo; cp <= ranges[ ri ].hi; ++cp )
    {
        /* Outside ASCII, a codepoint the face does not map is SKIPPED -- records are sparse by
           codepoint, so a hole costs nothing, whereas baking glyph 0 would store one .notdef box
           per hole.  Inside ASCII, glyph 0 still bakes, keeping the default output byte-identical
           to bakes made before ranges existed. */
        if ( cp > ORB_FONT_CP_LAST && stbtt_FindGlyphIndex( &font_info, (int)cp ) == 0 )
            continue;

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

/*==============================================================================================
    Fine (FreeType) bakes -- font_tool.exe spawned as a child process.

    font_tool lives next to the calling exe in bin/ and resolves the same inputs this library
    does (it links dev_font itself), so handing it the already-resolved absolute TTF path plus
    an explicit output path makes the child bake exactly what the caller asked for.  Output
    goes to assets/font_cache/ with an "_ft" tag -- never assets/font/, which is the shipped
    set and is staged wholesale by the ship pipeline.
==============================================================================================*/

#if OS_WINDOWS
    #define FONT_TOOL_EXE "font_tool.exe"
#else
    #define FONT_TOOL_EXE "font_tool"
#endif

/* Blocking font_tool run.  Failure text goes to `err` (not g_error) so background refine
   workers never race the main thread's error slot. */

static bool
fine_spawn( const char* ttf_abs, int size_px, const char* range_spec, const char* out_path,
            char* err, int err_size )
{
    char exe_dir[ DEV_PATH_MAX ];
    sys_exe_dir( exe_dir, sizeof( exe_dir ) );

    char cmd[ 2048 ];
    if ( range_spec && *range_spec )
        snprintf( cmd, sizeof( cmd ), "\"%s" PATH_SEP FONT_TOOL_EXE "\" \"%s\" %d \"%s\" -range=%s",
                  exe_dir, ttf_abs, size_px, out_path, range_spec );
    else
        snprintf( cmd, sizeof( cmd ), "\"%s" PATH_SEP FONT_TOOL_EXE "\" \"%s\" %d \"%s\"",
                  exe_dir, ttf_abs, size_px, out_path );

    char                 out[ 512 ] = { 0 };
    sys_process_result_t res        = { 0 };
    if ( !sys_process_run_capture( cmd, NULL, out, sizeof( out ), NULL, &res ) || !res.started )
    {
        snprintf( err, (size_t)err_size, "could not launch " FONT_TOOL_EXE " (expected in %s)",
                  exe_dir );
        return false;
    }
    if ( res.exit_code != 0 )
    {
        /* font_tool prints an "error:" line on failure -- surface its last output line. */
        const char* tail = out;
        for ( const char* p = out; *p; ++p )
            if ( ( *p == '\n' || *p == '\r' ) && p[ 1 ] && p[ 1 ] != '\n' && p[ 1 ] != '\r' )
                tail = p + 1;
        snprintf( err, (size_t)err_size, FONT_TOOL_EXE " failed (exit %d): %s",
                  res.exit_code, tail );
        return false;
    }
    return true;
}

/* Full structural check of a cached .orb_font this pipeline wrote: header fields sane and the
   file exactly as long as header + records + pixels claim.  Rejects the torn file a killed
   bake leaves behind, which the mtime freshness rule alone would accept. */

static bool
cache_file_valid( const char* path )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;

    orb_font_header_t hdr;
    memset( &hdr, 0, sizeof( hdr ) );
    bool ok = fread( &hdr, 1, ORB_FONT_HEADER_BASE_SIZE, f ) == ORB_FONT_HEADER_BASE_SIZE
           && hdr.magic == ORB_FONT_MAGIC
           && hdr.version >= 2u && hdr.version <= ORB_FONT_VERSION
           && hdr.glyph_count <= ORB_FONT_MAX_GLYPHS
           && hdr.atlas_w > 0 && hdr.atlas_h > 0;
    fclose( f );
    if ( !ok )
        return false;

    u32 hdr_bytes = ( hdr.version >= 4u ) ? (u32)sizeof( orb_font_header_t )
                                          : ORB_FONT_HEADER_BASE_SIZE;
    u32 expect    = hdr_bytes
                  + hdr.glyph_count * (u32)sizeof( orb_font_glyph_t )
                  + hdr.atlas_w * hdr.atlas_h;
    return sys_file_size( path ) == expect;
}

/* Cache path for a request at a quality tier:
   assets/font_cache/<stem>_<size>px[<rangetag>][_ft].orb_font */

static void
cache_path_make( const char* ttf_abs, int size_px, const char* range_spec, bool fine,
                 char* out, int out_size )
{
    char stem[ 64 ];
    derive_stem( ttf_abs, stem, sizeof( stem ) );

    char tag[ 48 ];
    dev_font_range_suffix( range_spec, tag, sizeof( tag ) );

    snprintf( out, (size_t)out_size, "%s" PATH_SEP "%s_%dpx%s%s.orb_font",
              g_rt.font_cache_dir, stem, size_px, tag, fine ? "_ft" : "" );
}

bool
dev_font_get_ex( const char* ttf_path, int size_px, dev_font_quality_t quality,
                 const char* range_spec, char* out_path, int out_path_size )
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

    dev_font_range_t ranges[ DEV_FONT_RANGE_MAX ];
    int              range_count = dev_font_range_parse( range_spec, ranges, DEV_FONT_RANGE_MAX );
    if ( range_count == 0 )
        return false;

    /* Resolve the source TTF to an absolute path that exists on disk. */

    char ttf_abs[ DEV_PATH_MAX ];
    if ( !resolve_ttf( ttf_path, ttf_abs, sizeof( ttf_abs ) ) )
        return false;

    u64 ttf_time = sys_file_time( ttf_abs );

    /* Fine tiers look for a fresh, structurally valid "_ft" bake first.  FINE_IF_CACHED never
       blocks on the child tool: a miss falls through to the fast stb bake below. */

    if ( quality != DEV_FONT_FAST )
    {
        char fine_path[ DEV_PATH_MAX ];
        cache_path_make( ttf_abs, size_px, range_spec, true, fine_path, sizeof( fine_path ) );

        u64 fine_time = sys_file_time( fine_path );
        if ( fine_time > 0 && ttf_time > 0 && fine_time >= ttf_time
             && cache_file_valid( fine_path ) )
        {
            snprintf( out_path, (size_t)out_path_size, "%s", fine_path );
            return true;
        }

        if ( quality == DEV_FONT_FINE )
        {
            sys_dir_make( g_rt.font_cache_dir );

            char err[ 256 ];
            if ( !fine_spawn( ttf_abs, size_px, range_spec, fine_path, err, sizeof( err ) ) )
            {
                set_error( "%s", err );
                return false;
            }
            if ( !cache_file_valid( fine_path ) )
            {
                set_error( "fine bake wrote an invalid file '%s'", fine_path );
                return false;
            }
            snprintf( out_path, (size_t)out_path_size, "%s", fine_path );
            return true;
        }
    }

    /* Fast tier: assets/font_cache/<stem>_<size>px[<rangetag>].orb_font */

    char cache_path[ DEV_PATH_MAX ];
    cache_path_make( ttf_abs, size_px, range_spec, false, cache_path, sizeof( cache_path ) );

    /* Cache hit: skip baking when the cached file is at least as new as the source TTF. */

    u64 cache_time = sys_file_time( cache_path );
    if ( cache_time > 0 && ttf_time > 0 && cache_time >= ttf_time )
    {
        snprintf( out_path, (size_t)out_path_size, "%s", cache_path );
        return true;
    }

    sys_dir_make( g_rt.font_cache_dir );   // create assets/font_cache/ on first bake

    if ( !bake_font( ttf_abs, size_px, ranges, range_count, cache_path ) )
        return false;

    snprintf( out_path, (size_t)out_path_size, "%s", cache_path );
    return true;
}

bool
dev_font_get( const char* ttf_path, int size_px, char* out_path, int out_path_size )
{
    return dev_font_get_ex( ttf_path, size_px, DEV_FONT_FAST, NULL, out_path, out_path_size );
}

/*==============================================================================================
    Background refine -- worker threads running fine_spawn.

    One table entry per in-flight refine keys on the fine cache path, so two kicks for the same
    (request, size, range) can never race font_tool over one output file.  The table and each
    entry's key are the only shared state; the mutex guards them.  Workers write no g_error and
    report through their own printf line.
==============================================================================================*/

#define REFINE_JOB_MAX 8

typedef struct
{
    u64  key;                          // FNV-1a of the fine cache path; 0 = slot free
    int  size_px;
    char ttf_abs[ DEV_PATH_MAX ];
    char spec   [ 64 ];                // range spec; "" = ASCII default
    char fine   [ DEV_PATH_MAX ];      // output path (the key's source string)

} refine_job_t;

static refine_job_t s_refine_jobs[ REFINE_JOB_MAX ];
static mutex_t      s_refine_mutex;
static bool         s_refine_mutex_ready;   // lazily set up on the main thread before any worker

static u64
hash_str64( const char* s )
{
    u64 h = 1469598103934665603ull;
    for ( ; *s; ++s )
    {
        h ^= (u8)*s;
        h *= 1099511628211ull;
    }
    return h ? h : 1u;   // 0 means "slot free" in the job table
}

static void
refine_worker( void* arg )
{
    refine_job_t* job = (refine_job_t*)arg;

    char err[ 256 ];
    if ( fine_spawn( job->ttf_abs, job->size_px, job->spec[ 0 ] ? job->spec : NULL,
                     job->fine, err, sizeof( err ) ) )
        printf( "[dev_font] refined '%s' %d px -> '%s'\n", job->ttf_abs, job->size_px, job->fine );
    else
        printf( "[dev_font] refine failed for '%s' %d px: %s\n", job->ttf_abs, job->size_px, err );

    mutex_lock( &s_refine_mutex );
    job->key = 0;
    mutex_unlock( &s_refine_mutex );
}

void
dev_font_refine_kick( const char* ttf_path, int size_px, const char* range_spec )
{
    if ( !g_rt.initialized || !ttf_path || !*ttf_path || size_px < 6 || size_px > 256 )
        return;

    char ttf_abs[ DEV_PATH_MAX ];
    if ( !resolve_ttf( ttf_path, ttf_abs, sizeof( ttf_abs ) ) )
        return;

    char fine_path[ DEV_PATH_MAX ];
    cache_path_make( ttf_abs, size_px, range_spec, true, fine_path, sizeof( fine_path ) );

    u64 fine_time = sys_file_time( fine_path );
    u64 ttf_time  = sys_file_time( ttf_abs );
    if ( fine_time > 0 && ttf_time > 0 && fine_time >= ttf_time && cache_file_valid( fine_path ) )
        return;   /* already fresh */

    /* Kicks only ever come from the main thread, so first-use init races nothing: no worker
       exists until after the mutex does. */
    if ( !s_refine_mutex_ready )
    {
        mutex_init( &s_refine_mutex );
        s_refine_mutex_ready = true;
    }

    u64 key = hash_str64( fine_path );

    mutex_lock( &s_refine_mutex );
    int slot = -1;
    for ( int i = 0; i < REFINE_JOB_MAX; ++i )
    {
        if ( s_refine_jobs[ i ].key == key )
        {
            mutex_unlock( &s_refine_mutex );   /* this exact refine is already in flight */
            return;
        }
        if ( s_refine_jobs[ i ].key == 0 && slot < 0 )
            slot = i;
    }
    if ( slot < 0 )
    {
        mutex_unlock( &s_refine_mutex );       /* table full -- a later kick retries */
        return;
    }

    refine_job_t* job = &s_refine_jobs[ slot ];
    job->key     = key;
    job->size_px = size_px;
    snprintf( job->ttf_abs, sizeof( job->ttf_abs ), "%s", ttf_abs );
    snprintf( job->spec,    sizeof( job->spec ),    "%s", range_spec ? range_spec : "" );
    snprintf( job->fine,    sizeof( job->fine ),    "%s", fine_path );
    mutex_unlock( &s_refine_mutex );

    sys_dir_make( g_rt.font_cache_dir );

    thread_t t = thread_create( refine_worker, job, 0 );
    if ( !thread_valid( t ) )
    {
        mutex_lock( &s_refine_mutex );
        job->key = 0;
        mutex_unlock( &s_refine_mutex );
        return;
    }
    thread_detach( t );
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
