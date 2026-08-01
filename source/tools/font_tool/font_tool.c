/*==============================================================================================

    tools/font_tool/font_tool.c -- Offline font atlas baker.

    Rasterizes a TTF/OTF face with FreeType and writes an .orb_font binary atlas the engine loads
    at runtime.  Rasterization is the only step that lives here -- packing, atlas blit and the file
    write are shared with the runtime stb baker through dev_font_bake_write(), so the two bakers
    cannot drift in layout.

    Usage:
        font_tool <input.ttf | "Font Name"> <size_px> [output.orb_font] [-sdf[=spread]] [-range=<spec>]
        font_tool info [<file.orb_font> | <dir>]...     -- print .orb_font header internals

    Input may be a path, a bare filename, or an installed font name; dev_font_resolve searches
    assets/font_source/ then the OS fonts.  With no output argument the path is derived as
    assets/font/<stem>_<size>px[_<range>][_sdf].orb_font.  The atlas is R8, sized to the smallest
    power-of-two square that fits every packed glyph.

    -range=<spec>   Bakes past the default ASCII span (U+0020..U+007E, 95 glyphs).  Comma-separated
                    preset names (ascii, latin1, latin, greek, cyrillic) and/or explicit LO-HI
                    codepoint spans.  Codepoints the face does not map are skipped -- glyph records
                    are sparse by codepoint, so holes cost nothing.

    -sdf[=spread]   Bakes a SIGNED DISTANCE FIELD rather than coverage, which is what makes text
                    scale and rotate cleanly (the GUI samples it LINEAR and recovers the edge with a
                    screen-space derivative).  It is the reason this tool is the offline path -- the
                    runtime stb baker stays coverage-only.  Field glyphs are bigger (each grows by
                    the spread on all four sides), so an SDF bake wants more atlas than the same
                    face in coverage.  The field is built HERE, by supersampling and a distance
                    transform, NOT by FreeType's sdf renderer -- see THE DISTANCE FIELD BAKER below
                    for why that module cannot be used.

    Link deps: freetype.lib (import lib for freetype.dll), dev_font (shared bake back-end)

    Fonts are usually in: C:\WINDOWS\FONTS\<font_name>.ttf

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>         /* sqrtf -- the distance transform */

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H     /* scale + rasterize the hinted outline for the field */

#include "tools/font_tool/orb_font.h"
#include "developer/dev_font/dev_font.h"   /* shared path resolver, output dirs, bake back-end */
#include "engine/sys/sys_host.h"           /* sys_dir_make, sys_file_glob */

/*==============================================================================================
    Constants
==============================================================================================*/

#if OS_WINDOWS
    #define OUT_SEP "\\"
#else
    #define OUT_SEP "/"
#endif

/* The default baked codepoint range and its glyph count come from the format contract
   (orb_font.h): ORB_FONT_CP_FIRST / ORB_FONT_CP_LAST / ORB_FONT_CP_COUNT. */

/* SDF spread, in pixels: how far the distance field reaches either side of the outline.  The range
   is inherited from FreeType's sdf module, which the baker no longer uses -- kept because it is a
   sane band and because every bake in the wild was made inside it.
   The spread costs atlas area -- every glyph rect grows by 2*spread on BOTH axes -- and buys the
   range over which effects that read distance (outline, glow) still have a gradient to read.  8 is
   ample for text at UI sizes; raise it only for a font meant to be scaled up hard.
   It is also the ENCODING's resolution: 127 byte steps span the spread, so a wider spread trades
   near-edge precision for reach (at 8, one step is 0.063 px). */
#define FONT_SDF_SPREAD_DEFAULT  8
#define FONT_SDF_SPREAD_MIN      2
#define FONT_SDF_SPREAD_MAX      32

#define FONT_PATH_MAX  512

/*==============================================================================================
    Path helpers
==============================================================================================*/

/* First character after the last separator -- equals `p` when there is no directory component. */
static const char*
path_base( const char* p )
{
    const char* base = p;
    for ( const char* q = p; *q; ++q )
        if ( *q == '/' || *q == '\\' )
            base = q + 1;
    return base;
}

static bool
path_has_orb_font_ext( const char* path )
{
    static const char ext[] = ".orb_font";
    size_t            n     = strlen( path );
    size_t            e     = sizeof( ext ) - 1;
    if ( n < e )
        return false;

    const char* tail = path + ( n - e );
    for ( size_t i = 0; i < e; ++i )
        if ( tolower( (unsigned char)tail[ i ] ) != ext[ i ] )
            return false;
    return true;
}

/* Create the parent directory of `path` if it does not already exist. */
static void
path_make_parent_dir( const char* path )
{
    char dir[ FONT_PATH_MAX ];
    snprintf( dir, sizeof( dir ), "%s", path );

    for ( int i = (int)strlen( dir ) - 1; i >= 0; --i )
        if ( dir[ i ] == '/' || dir[ i ] == '\\' )
        {
            dir[ i ] = '\0';
            sys_dir_make( dir );
            return;
        }
}

/*==============================================================================================
    Codepoint ranges -- what -range=<spec> parses into.

    No -range on the command line means exactly the ASCII contract from orb_font.h, so a default
    bake stays byte-identical to every bake made before the flag existed.  A spec is comma-
    separated tokens, each a preset name or an explicit LO[-HI] span (hex or decimal):
        -range=latin1        -range=latin,greek,cyrillic        -range=ascii,0x2022
    Spans are sorted and merged before baking, so overlapping presets never bake a glyph twice.
==============================================================================================*/

#define FONT_RANGE_MAX  32

typedef struct { uint32_t lo, hi; } cp_range_t;

static cp_range_t s_ranges[ FONT_RANGE_MAX ];
static int        s_range_count = 0;
static char       s_range_suffix[ 48 ];   /* derived-filename tag ("_latin1"); empty = default */

typedef struct
{
    const char* name;
    cp_range_t  span[ 2 ];
    int         count;

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
range_add( uint32_t lo, uint32_t hi )
{
    if ( s_range_count == FONT_RANGE_MAX )
    {
        fprintf( stderr, "error: -range has too many spans (max %d)\n", FONT_RANGE_MAX );
        return false;
    }
    s_ranges[ s_range_count ].lo = lo;
    s_ranges[ s_range_count ].hi = hi;
    ++s_range_count;
    return true;
}

/* One spec token: a preset name, a single codepoint, or "LO-HI" (strtoul base 0: hex or decimal). */
static bool
range_parse_token( const char* tok, size_t len )
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
                if ( !range_add( p->span[ s ].lo, p->span[ s ].hi ) )
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
    return range_add( (uint32_t)lo, (uint32_t)hi );

bad:
    fprintf( stderr, "error: bad -range token '%.*s' (preset name or LO[-HI], e.g. latin1 or 0xA0-0xFF)\n",
             (int)len, tok );
    return false;
}

static bool
range_parse_spec( const char* spec )
{
    const char* p = spec;
    while ( *p )
    {
        const char* q = p;
        while ( *q && *q != ',' )
            ++q;
        if ( !range_parse_token( p, (size_t)( q - p ) ) )
            return false;
        p = ( *q == ',' ) ? q + 1 : q;
    }
    if ( s_range_count == 0 )
    {
        fprintf( stderr, "error: -range spec is empty\n" );
        return false;
    }
    return true;
}

/* Sort by lo and merge touching spans, so overlapping tokens never bake a codepoint twice. */
static void
range_normalize( void )
{
    for ( int i = 1; i < s_range_count; ++i )     /* insertion sort -- the list is tiny */
    {
        cp_range_t r = s_ranges[ i ];
        int        j = i;
        while ( j > 0 && s_ranges[ j - 1 ].lo > r.lo )
        {
            s_ranges[ j ] = s_ranges[ j - 1 ];
            --j;
        }
        s_ranges[ j ] = r;
    }

    int w = 0;
    for ( int i = 1; i < s_range_count; ++i )
    {
        if ( s_ranges[ i ].lo <= s_ranges[ w ].hi + 1u )
        {
            if ( s_ranges[ i ].hi > s_ranges[ w ].hi )
                s_ranges[ w ].hi = s_ranges[ i ].hi;
        }
        else
        {
            s_ranges[ ++w ] = s_ranges[ i ];
        }
    }
    s_range_count = w + 1;
}

static uint32_t
range_codepoint_count( void )
{
    uint32_t n = 0;
    for ( int i = 0; i < s_range_count; ++i )
        n += s_ranges[ i ].hi - s_ranges[ i ].lo + 1u;
    return n;
}

/* Filename tag for a derived output path: '_' + the spec with anything unsafe mapped to '-'
   ("latin,greek" -> "_latin-greek"), so bakes of different ranges never overwrite each other. */
static void
range_make_suffix( const char* spec )
{
    int n                 = 0;
    s_range_suffix[ n++ ] = '_';
    for ( const char* p = spec; *p && n < (int)sizeof( s_range_suffix ) - 1; ++p )
    {
        char c = *p;
        if ( c >= 'A' && c <= 'Z' )
            c = (char)( c - 'A' + 'a' );
        s_range_suffix[ n++ ] = ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) ? c : '-';
    }
    s_range_suffix[ n ] = '\0';
}

/*==============================================================================================
    info subcommand -- read .orb_font headers and print their internals as a table.

    Read-only diagnostic companion to the bake path: `font_tool info [<file.orb_font> | <dir>]...`.
    With no argument it lists the engine font dir (assets/font); a .orb_font argument prints that
    one file; any other argument is treated as a directory and globbed for *.orb_font.
==============================================================================================*/

typedef struct { int count; } info_ctx_t;

/* Column titles, printed once above the rows. */
static void
info_print_header( void )
{
    printf( "\n%-40s %3s %5s %5s %5s %5s %5s %5s %7s %5s\n",
            "file", "ver", "aw", "ah", "size", "asc", "desc", "gap", "glyphs", "sdf" );
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

    /* Base first, tail only for v4+ -- the same versioned read the runtime loader does, so `info`
       reports a pre-v4 file exactly as the engine sees it (sdf_range 0) instead of showing whatever
       the first glyph record's leading bytes happen to be.  A truncated tail reads the same way. */
    const size_t base = ORB_FONT_HEADER_BASE_SIZE;
    const size_t tail = sizeof( orb_font_header_t ) - base;

    orb_font_header_t h;
    memset( &h, 0, sizeof( h ) );

    bool valid = fread( &h, 1, base, f ) == base && h.magic == ORB_FONT_MAGIC;
    if ( valid && h.version >= 4u && fread( (uint8_t*)&h + base, 1, tail, f ) != tail )
        h.sdf_range = 0;
    fclose( f );

    if ( !valid )
    {
        printf( "%-40s  (not an .orb_font)\n", label );
        return;
    }

    char sdf[ 8 ] = "-";
    if ( h.sdf_range )
        snprintf( sdf, sizeof( sdf ), "%u", h.sdf_range );

    printf( "%-40s %3u %5u %5u %5u %5d %5d %5d %7u %5s\n",
            label, h.version, h.atlas_w, h.atlas_h, h.font_size,
            h.ascent, h.descent, h.line_gap, h.glyph_count, sdf );
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
        char dir[ FONT_PATH_MAX ];
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
    THE DISTANCE FIELD BAKER

    Deliberately NOT FreeType's `sdf` renderer, and the reason is structural rather than a matter
    of tuning.  That module builds a field per CONTOUR and combines them with
    `min( max over clockwise contours, min over counter-clockwise contours )` (ftsdf.c,
    sdf_generate_with_overlaps), which has two failure modes it cannot be talked out of:

      * A FILLED ISLAND INSIDE A HOLE is not expressible.  The hole is a counter-clockwise
        contour, so the min VETOES every pixel inside it -- including the ones a later clockwise
        contour fills back in.  Cascadia Mono's DOTTED ZERO is exactly that shape, and its dot was
        absent from the field entirely: solid ink reading 63..100 where 128 is the outline.  Note
        this breaks the MAGNITUDES too, not just the signs, so no repair pass can recover it --
        the field around the dot encodes distance to the COUNTER's edge, and the dot's own edge is
        simply not in the data.
      * A CONTOUR THAT INTERSECTS ITSELF cannot be split apart, and FreeType's own documentation
        says so.  A crossbar running into a bowl ('e', 'a') slips the sign a few byte steps at the
        junction.

    What replaces it is the oldest and least clever method there is (Valve, Green 2007), and its
    virtue is that no step of it has an opinion about contours:

        1  rasterize the glyph at N times the target size with the ORDINARY rasterizer, whose
           nonzero-winding fill already resolves overlaps, self-intersections and islands
        2  threshold to a binary inside/outside mask
        3  exact Euclidean distance transform (Felzenszwalb-Huttenlocher) of both classes
        4  signed distance in fine pixels -> target pixels -> the byte encoding

    Accuracy: at N = 16 the edge is located to about 1/32 of a target pixel, while the byte
    encoding at spread 8 can only resolve 8/127 = 0.063 px.  So the METHOD is not the limit here,
    the FORMAT is -- which is the position worth being in.  What this does not give is MSDF: a
    single channel still rounds a sharp corner, and fixing that means median-of-3 over three
    channels (msdfgen), a fourth atlas in UNORM, and a shader change.

    THE SUPERSAMPLED RENDER IS THE SAME OUTLINE, NOT A SECOND ONE.  The obvious way to get it --
    set the pixel size to N times the target and load again -- silently bakes a DIFFERENT shape,
    because hinting grid-fits to whatever size is current and at N times the size it effectively
    stops.  Measured: 77 texels of the 32px face then disagreed with its coverage twin about which
    side of the outline they were on, not because the field was wrong but because the two bakes
    were no longer the same glyph.  So the outline is loaded ONCE at the target size, hinted, and
    then SCALED UP as geometry (FT_Outline_Transform) before rasterizing.  The field is thus the
    exact shape the coverage twin has, at N times the resolution, and the box it is rendered into
    is the ink box grown by `spread` -- so output texel (x, y) is fine cell (x*N + N/2, y*N + N/2)
    and the registration needs no bearing arithmetic at all.
==============================================================================================*/

#define SDF_FINE_MIN   4     /* supersample floor -- large faces would otherwise blow the grid */
#define SDF_FINE_MAX   16    /* and its ceiling: past this the byte encoding is the limit      */
#define SDF_INF        1e20f

/* One dimension of the exact squared distance transform: d[q] = min over p of (q-p)^2 + f[p].
   The lower envelope of a set of parabolas, walked in O(n) -- Felzenszwalb & Huttenlocher 2012.
   `v` (hull vertices) and `z` (hull breakpoints) are caller-owned scratch of n and n+1. */
static void
edt_1d( const float* f, float* d, int* v, float* z, int n )
{
    int k  = 0;
    v[ 0 ] = 0;
    z[ 0 ] = -SDF_INF;
    z[ 1 ] = +SDF_INF;

    for ( int q = 1; q < n; ++q )
    {
        float s;
        for ( ;; )
        {
            s = ( ( f[ q ] + (float)( q * q ) ) - ( f[ v[ k ] ] + (float)( v[ k ] * v[ k ] ) ) )
              / (float)( 2 * q - 2 * v[ k ] );
            if ( s <= z[ k ] && k > 0 )
                --k;
            else
                break;
        }
        ++k;
        v[ k ]     = q;
        z[ k ]     = s;
        z[ k + 1 ] = +SDF_INF;
    }

    k = 0;
    for ( int q = 0; q < n; ++q )
    {
        while ( z[ k + 1 ] < (float)q )
            ++k;
        float dq = (float)( q - v[ k ] );
        d[ q ]   = dq * dq + f[ v[ k ] ];
    }
}

/* Squared EDT of a w*h grid in place: columns, then rows.  `f` holds 0 at seed cells and SDF_INF
   elsewhere on entry, squared distance to the nearest seed on exit. */
static void
edt_2d( float* f, int w, int h, float* col, float* d, int* v, float* z )
{
    for ( int x = 0; x < w; ++x )
    {
        for ( int y = 0; y < h; ++y )
            col[ y ] = f[ (size_t)y * w + x ];
        edt_1d( col, d, v, z, h );
        for ( int y = 0; y < h; ++y )
            f[ (size_t)y * w + x ] = d[ y ];
    }
    for ( int y = 0; y < h; ++y )
    {
        float* row = f + (size_t)y * w;
        edt_1d( row, d, v, z, w );
        memcpy( row, d, (size_t)w * sizeof( float ) );
    }
}

/* Rasterize the slot's CURRENT (already hinted, target-size) outline at n times scale into the
   ow x oh output box grown from the ink box by `spread`.  The outline is scaled as geometry and
   translated so the box's top-left lands on the mask's (0,0); `cleft`/`ctop` are the ink box's
   bearings, in target pixels, y positive up.  Caller frees. */
static uint8_t*
sdf_render_fine( FT_Library lib, FT_Outline* outline,
                 int ow, int oh, int cleft, int ctop, int spread, int n )
{
    int      gw   = ow * n;
    int      gh   = oh * n;
    uint8_t* mask = (uint8_t*)calloc( (size_t)gw * (size_t)gh, 1 );
    if ( !mask )
        return NULL;

    FT_Matrix m = { (FT_Fixed)n << 16, 0, 0, (FT_Fixed)n << 16 };
    FT_Outline_Transform( outline, &m );

    /* Box edges in target px (y up): left is the ink box pushed out by the spread, bottom is the
       top minus the full box height.  Shift the (already n-scaled) outline so those land on 0. */
    int box_left   = cleft - spread;
    int box_bottom = ( ctop + spread ) - oh;
    FT_Outline_Translate( outline, -(FT_Pos)box_left * 64 * n, -(FT_Pos)box_bottom * 64 * n );

    FT_Bitmap bm;
    memset( &bm, 0, sizeof( bm ) );
    bm.width      = (unsigned int)gw;
    bm.rows       = (unsigned int)gh;
    bm.pitch      = gw;
    bm.num_grays  = 256;
    bm.pixel_mode = FT_PIXEL_MODE_GRAY;
    bm.buffer     = mask;

    if ( FT_Outline_Get_Bitmap( lib, outline, &bm ) )
    {
        free( mask );
        return NULL;
    }
    return mask;
}

/* Turn a supersampled coverage mask into the field.  `fine` is (ow*n) x (oh*n) and covers exactly
   the ow x oh output box, so a texel's centre is fine cell (x*n + n/2, y*n + n/2) and there is no
   registration to get wrong.  Returns NULL only on an allocation failure.  Caller frees. */
static uint8_t*
sdf_field_from_mask( const uint8_t* fine, int ow, int oh, int n, int spread )
{
    int gw = ow * n;
    int gh = oh * n;

    uint8_t* out  = (uint8_t*)malloc( (size_t)ow * (size_t)oh );
    float*   din  = (float*)malloc( (size_t)gw * (size_t)gh * sizeof( float ) );
    float*   dout = (float*)malloc( (size_t)gw * (size_t)gh * sizeof( float ) );
    int      mx   = gw > gh ? gw : gh;
    float*   col  = (float*)malloc( (size_t)mx * sizeof( float ) );
    float*   dsc  = (float*)malloc( (size_t)mx * sizeof( float ) );
    int*     vsc  = (int*)malloc( (size_t)mx * sizeof( int ) );
    float*   zsc  = (float*)malloc( (size_t)( mx + 1 ) * sizeof( float ) );

    if ( !out || !din || !dout || !col || !dsc || !vsc || !zsc )
    {
        free( out );
        out = NULL;
        goto done;
    }

    /* Two transforms off one mask: `din` seeds on OUTSIDE cells, so an inside cell learns its
       distance to the boundary, and `dout` seeds on INSIDE cells for the reverse. */
    for ( int i = 0; i < gw * gh; ++i )
    {
        bool inside = fine[ i ] >= 128;
        din [ i ]   = inside ? SDF_INF : 0.0f;
        dout[ i ]   = inside ? 0.0f    : SDF_INF;
    }
    edt_2d( din,  gw, gh, col, dsc, vsc, zsc );
    edt_2d( dout, gw, gh, col, dsc, vsc, zsc );

    for ( int y = 0; y < oh; ++y )
    {
        for ( int x = 0; x < ow; ++x )
        {
            size_t i  = (size_t)( y * n + n / 2 ) * gw + ( x * n + n / 2 );
            bool   in = ( dout[ i ] == 0.0f );    /* seeded 0 exactly on the inside cells */

            /* Distances run centre to centre, so the boundary sits half a fine pixel inside the
               measurement; take that off, then convert fine pixels to target pixels. */
            float dist = in ? ( sqrtf( din[ i ] ) - 0.5f ) : -( sqrtf( dout[ i ] ) - 0.5f );
            dist      /= (float)n;

            /* FreeType's encoding kept byte for byte -- 128 is the outline and 127 steps span
               `spread` px -- so nothing downstream of the format had to learn a second one. */
            int b = (int)( 128.0f + dist * 127.0f / (float)spread + 0.5f );
            if ( b < 0 )   b = 0;
            if ( b > 255 ) b = 255;
            out[ (size_t)y * ow + x ] = (uint8_t)b;
        }
    }

done:
    free( din ); free( dout ); free( col ); free( dsc ); free( vsc ); free( zsc );
    return out;
}

/*==============================================================================================
    Rasterization -- two passes over the face, filling the scratch the shared back-end packs.
==============================================================================================*/

/* Rasterized glyphs handed to dev_font_bake_write (BSS, not the stack).  Sized to the format cap
   rather than the ASCII count, because -range feeds extended sets through here; main rejects a
   spec wider than the cap before either pass runs, which is what bounds the writes below. */
static dev_font_glyph_t  s_glyphs[ ORB_FONT_MAX_GLYPHS ];

/*----------------------------------------------------------------------------------------------
    PASS 1 -- at the TARGET size, hinted.  This is the authority for every NUMBER a glyph carries:
    advance, bearings and the ink box.  An SDF bake takes its metrics from here too and borrows
    only the SHAPE from pass 2, which is what keeps the two bakes of one face measuring identically
    down to the pixel.
----------------------------------------------------------------------------------------------*/

static bool
rasterize_coverage( FT_Face face, uint32_t* out_count )
{
    uint32_t count = 0;

    memset( s_glyphs, 0, sizeof( s_glyphs ) );

    for ( int ri = 0; ri < s_range_count; ++ri )
    for ( uint32_t cp = s_ranges[ ri ].lo; cp <= s_ranges[ ri ].hi; ++cp )
    {
        FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)cp );

        /* Outside ASCII, a codepoint the face does not map is SKIPPED -- records are sparse by
           codepoint, so a hole costs nothing, whereas loading glyph 0 would bake one .notdef box
           per hole.  Inside ASCII, glyph 0 still bakes, keeping the default output byte-identical
           to bakes made before -range existed. */
        if ( glyph_idx == 0 && cp > ORB_FONT_CP_LAST )
            continue;
        if ( FT_Load_Glyph( face, glyph_idx, FT_LOAD_RENDER ) )
            continue;

        FT_GlyphSlot      g = face->glyph;
        dev_font_glyph_t* r = &s_glyphs[ count++ ];

        r->codepoint = cp;
        r->w         = (int)g->bitmap.width;
        r->h         = (int)g->bitmap.rows;
        r->advance   = (int)( g->advance.x >> 6 );   // 26.6 fixed point; >> 6 gives whole pixels

        /* horiBearing describes the OUTLINE and bitmap_left/_top the RASTERIZED box; for a plain
           coverage render they agree, and horiBearing is what every earlier bake used, so it stays
           the source here.  Pass 2 grows both by `spread` for a field. */
        r->bearing_x = (int)( g->metrics.horiBearingX >> 6 );
        r->bearing_y = (int)( g->metrics.horiBearingY >> 6 );   // FT convention: positive = above baseline

        if ( r->w <= 0 || r->h <= 0 )
            continue;                                /* whitespace: metrics only, no pixels */

        r->bitmap = (uint8_t*)malloc( (size_t)r->w * r->h );
        if ( !r->bitmap )
        {
            fprintf( stderr, "error: out of memory rasterizing U+%04X\n", cp );
            *out_count = count;                      /* caller still frees what was allocated */
            return false;
        }

        /* Row by row: FreeType's pitch may exceed the width because rows are aligned. */
        for ( int row = 0; row < r->h; ++row )
            memcpy( r->bitmap + (size_t)row * r->w,
                    g->bitmap.buffer + (size_t)row * (uint32_t)g->bitmap.pitch,
                    (size_t)r->w );
    }

    *out_count = count;
    return true;
}

/*----------------------------------------------------------------------------------------------
    PASS 2 -- the field, replacing each glyph's coverage bitmap in place.  A second pass rather
    than work folded into the first because FT_LOAD_RENDER above consumed the outline, and this
    needs it back.  The pixel size never changes: the supersampling is a transform on the outline,
    not a reload at another size.
----------------------------------------------------------------------------------------------*/

static bool
rasterize_sdf( FT_Library ft, FT_Face face, uint32_t count, int spread, int size_px )
{
    /* Aim for a fine grid around 512 cells tall, clamped: the floor keeps small faces accurate,
       the ceiling keeps large ones from blowing up a grid the byte encoding cannot use anyway. */
    int fine_n = 512 / size_px;
    if ( fine_n < SDF_FINE_MIN ) fine_n = SDF_FINE_MIN;
    if ( fine_n > SDF_FINE_MAX ) fine_n = SDF_FINE_MAX;

    for ( uint32_t i = 0; i < count; ++i )
    {
        dev_font_glyph_t* r = &s_glyphs[ i ];
        if ( r->w <= 0 || r->h <= 0 )
            continue;                       /* whitespace: no ink, so no field */

        FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)r->codepoint );
        if ( FT_Load_Glyph( face, glyph_idx, FT_LOAD_DEFAULT ) )
            continue;

        int ow = r->w + 2 * spread;
        int oh = r->h + 2 * spread;

        uint8_t* mask  = sdf_render_fine( ft, &face->glyph->outline, ow, oh,
                                          r->bearing_x, r->bearing_y, spread, fine_n );
        uint8_t* field = mask ? sdf_field_from_mask( mask, ow, oh, fine_n, spread ) : NULL;
        free( mask );

        if ( !field )
        {
            fprintf( stderr, "error: cannot build the distance field for U+%04X\n", r->codepoint );
            return false;
        }

        /* The field IS the ink box grown by `spread` on all four sides, so the bearings move with
           it -- the same adjustment FreeType's own sdf renderer used to make, which is why the
           runtime loader needed no change when the generator was replaced. */
        free( r->bitmap );
        r->bitmap     = field;
        r->bearing_x -= spread;
        r->bearing_y += spread;
        r->w          = ow;
        r->h          = oh;
    }

    printf( "[font_tool] distance field: %dx supersample, spread %d px\n", fine_n, spread );
    return true;
}

/*==============================================================================================
    Command line
==============================================================================================*/

typedef struct
{
    const char* ttf_arg;    // input as typed: path, bare filename, or installed font name
    const char* out_arg;    // explicit output path; NULL = derive one
    int         size_px;    // glyph height
    uint32_t    sdf_range;  // 0 = coverage bake, > 0 = distance field with this spread

} cli_t;

static void
usage_print( void )
{
    fprintf( stderr,
        "usage: font_tool <input.ttf | \"Font Name\"> <size_px> [output.orb_font] [-sdf[=spread]] [-range=<spec>]\n"
        "       font_tool info [<file.orb_font> | <dir>]...   (print header internals)\n"
        "       input may be a path, a bare filename, or an installed font name\n"
        "       output defaults to assets/font/<name>_<size>px[_<range>][_sdf].orb_font\n"
        "       -sdf bakes a distance field (default spread %d px) instead of coverage\n"
        "       -range bakes beyond ASCII: presets ascii|latin1|latin|greek|cyrillic\n"
        "              and/or LO-HI codepoint spans, comma-separated (default: ascii)\n",
        FONT_SDF_SPREAD_DEFAULT );
}

/* Flags may appear anywhere; whatever is left over is <input> <size_px> [output].  -range fills
   the span list as it parses, so cli_t carries no range of its own.  Prints its own diagnostic
   and returns false on any bad argument. */
static bool
cli_parse( int argc, char** argv, cli_t* cli )
{
    const char* pos[ 3 ];
    int         npos = 0;

    memset( cli, 0, sizeof( *cli ) );

    for ( int i = 1; i < argc; ++i )
    {
        const char* a = argv[ i ];

        if ( strncmp( a, "-range", 6 ) == 0 )
        {
            if ( a[ 6 ] != '=' || !a[ 7 ] )
            {
                fprintf( stderr, "error: -range needs a spec, e.g. -range=latin1\n" );
                return false;
            }
            if ( !range_parse_spec( a + 7 ) )
                return false;
            range_make_suffix( a + 7 );
            continue;
        }

        if ( strncmp( a, "-sdf", 4 ) == 0 && ( a[ 4 ] == '\0' || a[ 4 ] == '=' ) )
        {
            int spread = ( a[ 4 ] == '=' ) ? atoi( a + 5 ) : FONT_SDF_SPREAD_DEFAULT;
            if ( spread < FONT_SDF_SPREAD_MIN || spread > FONT_SDF_SPREAD_MAX )
            {
                fprintf( stderr, "error: -sdf spread must be %d..%d\n",
                         FONT_SDF_SPREAD_MIN, FONT_SDF_SPREAD_MAX );
                return false;
            }
            cli->sdf_range = (uint32_t)spread;
            continue;
        }

        if ( a[ 0 ] == '-' )
        {
            fprintf( stderr, "error: unknown option '%s'\n", a );
            return false;
        }

        if ( npos == (int)( sizeof( pos ) / sizeof( pos[ 0 ] ) ) )
        {
            fprintf( stderr, "error: too many arguments (at '%s')\n", a );
            return false;
        }
        pos[ npos++ ] = a;
    }

    if ( npos < 2 )
    {
        usage_print();
        return false;
    }

    cli->ttf_arg = pos[ 0 ];
    cli->size_px = atoi( pos[ 1 ] );
    cli->out_arg = ( npos == 3 ) ? pos[ 2 ] : NULL;

    if ( cli->size_px < 6 || cli->size_px > 256 )
    {
        fprintf( stderr, "error: size_px must be 6..256\n" );
        return false;
    }
    return true;
}

/* Where the atlas lands.  An output argument with a directory component is used as-is; a bare
   filename is redirected into assets/font/; no argument derives <stem>_<size>px there.  The
   _<range> and _sdf tags keep the bakes of one face at one size from overwriting each other --
   they are different assets, and a build may well want several.  A missing extension is added. */
static bool
out_path_resolve( const cli_t* cli, const char* ttf_abs, const char* font_dir,
                  char* buf, size_t cap )
{
    int n;

    if ( cli->out_arg )
    {
        bool has_dir = ( path_base( cli->out_arg ) != cli->out_arg );

        n = has_dir ? snprintf( buf, cap, "%s", cli->out_arg )
                    : snprintf( buf, cap, "%s" OUT_SEP "%s", font_dir, cli->out_arg );

        if ( n > 0 && n < (int)cap && !path_has_orb_font_ext( buf ) )
            n += snprintf( buf + n, cap - (size_t)n, ".orb_font" );
    }
    else
    {
        const char* base     = path_base( ttf_abs );
        size_t      stem_len = strlen( base );
        for ( size_t i = stem_len; i-- > 0; )
            if ( base[ i ] == '.' ) { stem_len = i; break; }

        n = snprintf( buf, cap, "%s" OUT_SEP "%.*s_%dpx%s%s.orb_font",
                      font_dir, (int)stem_len, base, cli->size_px,
                      s_range_suffix, cli->sdf_range ? "_sdf" : "" );
    }

    if ( n <= 0 || n >= (int)cap )
    {
        fprintf( stderr, "error: output path too long\n" );
        return false;
    }
    return true;
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

    cli_t cli;
    if ( !cli_parse( argc, argv, &cli ) )
        return 1;

    /* No -range on the command line: exactly the ASCII contract, as every bake before the flag. */
    if ( s_range_count == 0 )
        range_add( ORB_FONT_CP_FIRST, ORB_FONT_CP_LAST );
    range_normalize();

    uint32_t cp_total = range_codepoint_count();
    if ( cp_total > ORB_FONT_MAX_GLYPHS )
    {
        fprintf( stderr, "error: -range spans %u codepoints, format cap is %u\n",
                 cp_total, ORB_FONT_MAX_GLYPHS );
        return 1;
    }

    /* dev_font owns both ends of the file system: it resolves a bare filename or friendly name
       ("Cascadia Mono") against assets/font_source/ and the OS fonts -- the same search the runtime
       stb baker does -- and it names the output dir.  Final (FreeType) bakes land in assets/font/,
       parallel to the assets/font_cache/ the runtime uses, so the two paths cannot drift. */
    dev_font_init( NULL );

    char ttf_abs[ FONT_PATH_MAX ];
    if ( !dev_font_resolve( cli.ttf_arg, ttf_abs, sizeof( ttf_abs ) ) )
    {
        fprintf( stderr, "error: %s\n", dev_font_last_error() );
        return 1;
    }

    char font_dir[ FONT_PATH_MAX ];
    if ( !dev_font_dir( font_dir, sizeof( font_dir ) ) )
    {
        fprintf( stderr, "error: dev_font not initialized\n" );
        return 1;
    }

    char out_path[ FONT_PATH_MAX ];
    if ( !out_path_resolve( &cli, ttf_abs, font_dir, out_path, sizeof( out_path ) ) )
        return 1;
    path_make_parent_dir( out_path );

    /*------------------------------------------------------------------------------------------
        Rasterize with FreeType into the shared dev_font_glyph_t scratch, then hand off to
        dev_font_bake_write() for packing, atlas blit and the .orb_font write -- the same back-end
        the runtime stb baker uses, so both produce the same layout.
    ------------------------------------------------------------------------------------------*/

    FT_Library ft;
    if ( FT_Init_FreeType( &ft ) )
    {
        fprintf( stderr, "error: FT_Init_FreeType failed\n" );
        return 1;
    }

    FT_Face face;
    if ( FT_New_Face( ft, ttf_abs, 0, &face ) )
    {
        fprintf( stderr, "error: cannot load font '%s'\n", ttf_abs );
        FT_Done_FreeType( ft );
        return 1;
    }

    FT_Set_Pixel_Sizes( face, 0, (FT_UInt)cli.size_px );   /* 0 width means "same as height" */

    /* Global metrics -- FreeType uses 26.6 fixed point, >> 6 converts to integer pixels. */
    int32_t ascent   = (int32_t)( face->size->metrics.ascender  >> 6 );
    int32_t descent  = (int32_t)( face->size->metrics.descender >> 6 );
    int32_t line_gap = (int32_t)( face->size->metrics.height    >> 6 ) - ascent + descent;

    uint32_t count = 0;
    bool     ok    = rasterize_coverage( face, &count );

    if ( ok && cli.sdf_range )
        ok = rasterize_sdf( ft, face, count, (int)cli.sdf_range, cli.size_px );

    FT_Done_Face( face );
    FT_Done_FreeType( ft );

    if ( ok && s_range_suffix[ 0 ] )
        printf( "[font_tool] range: %u codepoints in %d spans, %u glyphs mapped by the face\n",
                cp_total, s_range_count, count );

    if ( ok )
    {
        ok = dev_font_bake_write( out_path, s_glyphs, count, ascent, descent, line_gap,
                                  cli.size_px, cli.sdf_range, ttf_abs );
        if ( !ok )
            fprintf( stderr, "error: %s\n", dev_font_last_error() );
    }

    for ( uint32_t i = 0; i < count; ++i )
        free( s_glyphs[ i ].bitmap );

    return ok ? 0 : 1;
}

/*============================================================================================*/
// clang-format on
