/*==============================================================================================

    tools/font_tool/font_tool.c -- Offline font atlas baker.

    Rasterizes a TTF/OTF font at a given pixel size using FreeType, packs the
    glyph bitmaps into a texture atlas with stb_rect_pack, and writes an
    .orb_font binary atlas file the engine loads at runtime.

    Usage:
        font_tool.exe <input.ttf> <size_px> <output.orb_font> [-sdf[=spread]]
        font_tool.exe info [<file.orb_font> | <dir>]...   -- print .orb_font header internals

    Bakes the ASCII printable range U+0020 (space) .. U+007E (tilde), 95 glyphs.
    .orb_font output atlas is R8 grayscale, sized to the smallest power-of-two square that fits
    every packed glyph.  Rasterization is the only font_tool-specific step -- packing, atlas
    blit and the .orb_font write are shared with the runtime baker via dev_font_bake_write().

    -sdf bakes a SIGNED DISTANCE FIELD rather than coverage, which is what makes text scale and
    rotate cleanly (the GUI samples it LINEAR and recovers the edge with a screen-space
    derivative).  It is the reason this tool is the offline path -- the runtime stb baker stays
    coverage-only.  Distance-field glyphs are bigger (each grows by the spread on all four sides),
    so an SDF bake wants more atlas than the same face in coverage.  The field is built HERE, by
    supersampling and a distance transform, NOT by FreeType's sdf renderer -- see the baker
    section below for why that module cannot be used.

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
       the first glyph record's leading bytes happen to be. */
    orb_font_header_t h;
    memset( &h, 0, sizeof( h ) );
    size_t got = fread( &h, 1, ORB_FONT_HEADER_BASE_SIZE, f );

    if ( got != ORB_FONT_HEADER_BASE_SIZE || h.magic != ORB_FONT_MAGIC )
    {
        fclose( f );
        printf( "%-40s  (not an .orb_font)\n", label );
        return;
    }
    if ( h.version >= 4u )
        fread( (uint8_t*)&h + ORB_FONT_HEADER_BASE_SIZE, 1,
               sizeof( h ) - ORB_FONT_HEADER_BASE_SIZE, f );
    fclose( f );

    char sdf[ 8 ];
    if ( h.sdf_range ) snprintf( sdf, sizeof( sdf ), "%u", h.sdf_range );
    else               snprintf( sdf, sizeof( sdf ), "-" );

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

/* Turn a supersampled coverage mask into the field.  `fine` is (ow*n) x (oh*n) and covers exactly
   the ow x oh output box, so a texel's centre is fine cell (x*n + n/2, y*n + n/2) and there is no
   registration to get wrong.  Returns NULL only on an allocation failure. */
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

    /* Optional -sdf[=spread], accepted anywhere in the argument list and filtered out here so the
       positional parsing below stays exactly as it was. */
    uint32_t sdf_range = 0;
    char*    fargv[ 8 ];
    int      fargc = 0;
    for ( int i = 0; i < argc; ++i )
    {
        if ( strncmp( argv[ i ], "-sdf", 4 ) == 0
             && ( argv[ i ][ 4 ] == '\0' || argv[ i ][ 4 ] == '=' ) )
        {
            int v = ( argv[ i ][ 4 ] == '=' ) ? atoi( argv[ i ] + 5 ) : FONT_SDF_SPREAD_DEFAULT;
            if ( v < FONT_SDF_SPREAD_MIN || v > FONT_SDF_SPREAD_MAX )
            {
                fprintf( stderr, "error: -sdf spread must be %d..%d\n",
                         FONT_SDF_SPREAD_MIN, FONT_SDF_SPREAD_MAX );
                return 1;
            }
            sdf_range = (uint32_t)v;
            continue;
        }
        if ( fargc == (int)( sizeof( fargv ) / sizeof( fargv[ 0 ] ) ) )
            break;
        fargv[ fargc++ ] = argv[ i ];
    }
    argc = fargc;
    argv = fargv;

    if ( argc < 3 || argc > 4 )
    {
        fprintf( stderr, "usage: font_tool <input.ttf | \"Font Name\"> <size_px> [output.orb_font] [-sdf[=spread]]\n" );
        fprintf( stderr, "       font_tool info [<file.orb_font> | <dir>]...   (print header internals)\n" );
        fprintf( stderr, "       input may be a path, a bare filename, or an installed font name\n" );
        fprintf( stderr, "       output defaults to assets/font/<name>_<size>px[_sdf].orb_font\n" );
        fprintf( stderr, "       -sdf bakes a distance field (default spread %d px) instead of coverage\n",
                 FONT_SDF_SPREAD_DEFAULT );
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

        /* The _sdf suffix keeps the two bakes of one face at one size from overwriting each other --
           they are different assets, and a build may well want both. */
        int n = snprintf( s_out_buf, sizeof( s_out_buf ),
                          "%s" OUT_SEP "%.*s_%dpx%s.orb_font", font_dir, (int)stem_len, base, size_px,
                          sdf_range ? "_sdf" : "" );
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

    /*------------------------------------------------------------------------------------------
        PASS 1 -- at the TARGET size, hinted.  This is the authority for every NUMBER a glyph
        carries: advance, bearings and the ink box.  An SDF bake takes its metrics from here too
        and only borrows the shape from pass 2, which is what keeps the two bakes of one face
        measuring identically down to the pixel.
    ------------------------------------------------------------------------------------------*/

    for ( uint32_t cp = ORB_FONT_CP_FIRST; cp <= ORB_FONT_CP_LAST; ++cp )
    {
        FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)cp );

        if ( FT_Load_Glyph( face, glyph_idx, FT_LOAD_RENDER ) )
            continue;

        FT_GlyphSlot g = face->glyph;

        dev_font_glyph_t* r = &s_glyphs[ raw_count++ ];

        r->codepoint = cp;
        r->w         = (int)g->bitmap.width;
        r->h         = (int)g->bitmap.rows;
        r->advance   = (int)( g->advance.x >> 6 );

        /* horiBearing describes the OUTLINE and bitmap_left/_top the RASTERIZED box; for a plain
           coverage render they agree, and horiBearing is what every earlier bake used, so it stays
           the source here.  Pass 2 grows both by `spread` for a field. */
        r->bearing_x = (int)( g->metrics.horiBearingX >> 6 );
        r->bearing_y = (int)( g->metrics.horiBearingY >> 6 );   // FT convention: positive = above baseline

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

    /*------------------------------------------------------------------------------------------
        PASS 2 -- the field.  A second pass rather than work folded into the first because
        FT_LOAD_RENDER above consumed the outline, and this needs it back.  The pixel size never
        changes: the supersampling is a transform on the outline, not a reload at another size.
    ------------------------------------------------------------------------------------------*/

    if ( sdf_range )
    {
        int fine_n = 512 / size_px;
        if ( fine_n < SDF_FINE_MIN ) fine_n = SDF_FINE_MIN;
        if ( fine_n > SDF_FINE_MAX ) fine_n = SDF_FINE_MAX;

        int spread = (int)sdf_range;

        for ( uint32_t i = 0; i < raw_count; ++i )
        {
            dev_font_glyph_t* r = &s_glyphs[ i ];
            if ( r->w <= 0 || r->h <= 0 )
                continue;                       /* whitespace: no ink, so no field */

            FT_UInt glyph_idx = FT_Get_Char_Index( face, (FT_ULong)r->codepoint );
            if ( FT_Load_Glyph( face, glyph_idx, FT_LOAD_DEFAULT ) )
                continue;

            int ow = r->w + 2 * spread;
            int oh = r->h + 2 * spread;

            uint8_t* mask = sdf_render_fine( ft, &face->glyph->outline, ow, oh,
                                             r->bearing_x, r->bearing_y, spread, fine_n );
            uint8_t* field = mask ? sdf_field_from_mask( mask, ow, oh, fine_n, spread ) : NULL;
            free( mask );

            if ( !field )
            {
                fprintf( stderr, "error: cannot build the distance field for U+%04X\n",
                         r->codepoint );
                FT_Done_Face( face );
                FT_Done_FreeType( ft );
                return 1;
            }

            /* The field IS the ink box grown by `spread` on all four sides, so the bearings move
               with it -- the same adjustment FreeType's own sdf renderer used to make, which is
               why the runtime loader needed no change when the generator was replaced. */
            free( r->bitmap );
            r->bitmap     = field;
            r->bearing_x -= spread;
            r->bearing_y += spread;
            r->w          = ow;
            r->h          = oh;
        }

        printf( "[font_tool] distance field: %dx supersample, spread %u px\n", fine_n, sdf_range );
    }

    FT_Done_Face( face );
    FT_Done_FreeType( ft );

    bool ok = dev_font_bake_write( out_path, s_glyphs, raw_count,
                                   ascent, descent, line_gap, size_px, sdf_range, ttf_path );
    if ( !ok )
        fprintf( stderr, "error: %s\n", dev_font_last_error() );

    for ( uint32_t i = 0; i < raw_count; ++i )
        free( s_glyphs[ i ].bitmap );

    return ok ? 0 : 1;
}

/*============================================================================================*/
// clang-format on