/*==============================================================================================

    tools/image_tool/image_tool.c -- Offline 2D image utility.

    Thin CLI front-end over the dev_image library (developer/dev_image).  The founding job is
    cutting AI-generated sprite sheets into individual transparent PNGs so single images can be
    picked out selectively instead of cropped by hand; further image verbs land here as needed.

    Usage:
        image_tool split <sheet> <cols> <rows> [-out <dir>] [-key]
        image_tool key   <image> [-out <path>] [-pad <px>]
        image_tool svg   <input.svg | dir> [-size <px>] [-margin <px>] [-out <path | dir>]
        image_tool info  <image>...

    split -- cut <sheet> into cols * rows equal cells and write each cell as
             <dir>/<stem>_r<row>_c<col>.png.  The sheet must divide evenly by the grid.
             -out <dir>   output directory (default: <sheet path minus extension>/)
             -key         derive alpha from luminance (alpha = max rgb), knocking an opaque
                          dark background out to transparency.  Suggested automatically when
                          the sheet carries no alpha of its own.

    key   -- single-image version of -key: derive alpha from luminance (alpha = max rgb), so a
             flat dark background becomes real transparency instead of opaque black.  The usual
             fix for a generated icon that reads as "a solid square" once loaded -- the engine's
             icon loader trusts a real alpha channel over colour, so a background that is merely
             dark but still opaque gets read as solid ink.
             -out <path>  output file (default: <image path minus extension>_keyed.png)
             -pad <px>    add a fully transparent margin of <px> on every side afterward.  An
                          SDF bake needs an outside to fall off into (icon_register_sdf); art
                          keyed right up to its own edges bakes with a hard, unantialiased cut.

    svg   -- rasterize an SVG (or every .svg in a directory) to transparent PNG.  Vector art
             needs no keying: a path is either filled or it is not, so the output carries
             clean alpha coverage directly.  Small sizes feed the gui coverage icon path;
             a large size with a margin (e.g. -size 1024 -margin 64) produces a valid SDF
             bake source (icon_register_sdf needs an outside to fall off into).
             -size <px>    longest edge of the output, margins included (default: 32)
             -margin <px>  transparent border on every side, carved out of -size (default: 0)
             -out <path>   output file for a single input (default: <input minus extension>.png)
                           or output directory for a directory input (default: the input dir)

    icons -- batch-rasterize the built-in icon set from a manifest (config/icons.manifest): one
             line per icon, run through the same SVG raster as `svg` above.  This is the IMPORT
             step: it turns a raw source under assets/ui/icon/ into content under
             content/ui/icon/ (the two trees mirror each other).  Re-run whenever the source art
             changes; nothing bakes automatically.
                 <name> <source.svg> [-size <px>] [-margin <px>] [-out <path>]
             <name>       the gui icon registry key; also the default output stem
             <source.svg> resolved against assets/ui/icon/
             -size, -margin  default to 256 / 16 -- generous SDF bake headroom (see `svg` above)
             -out         override the default content/ui/icon/<name>.png

    info  -- print dimensions, grid hints, and whether the image carries transparency.

    Input is any stb_image format (PNG, JPG, BMP, TGA, GIF, ...) or SVG; output is always PNG.

    Link deps: dev_image (pixel work), dev_vector (svg parse + rasterize),
               sys (directory creation, svg globbing)

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "developer/dev_image/dev_image.h"
#include "developer/dev_vector/dev_vector.h"

/*==============================================================================================
    default_out_dir -- <sheet path minus extension>; "art/sheet.png" -> "art/sheet".
==============================================================================================*/

static void
default_out_dir( const char* sheet_path, char* out, int out_size )
{
    snprintf( out, (size_t)out_size, "%s", sheet_path );

    /* Strip the extension of the last path component only. */
    char* base = out;
    for ( char* p = out; *p; ++p )
        if ( *p == '/' || *p == '\\' )
            base = p + 1;
    char* dot = strrchr( base, '.' );
    if ( dot && dot > base )
        *dot = '\0';
}

/*==============================================================================================
    run_split
==============================================================================================*/

static int
run_split( int argc, char** argv )
{
    if ( argc < 3 )
    {
        fprintf( stderr, "usage: image_tool split <sheet> <cols> <rows> [-out <dir>] [-key]\n" );
        return 1;
    }

    const char* sheet_path = argv[ 0 ];
    int         cols       = atoi( argv[ 1 ] );
    int         rows       = atoi( argv[ 2 ] );

    const char* out_arg = NULL;
    u32         flags   = 0;
    for ( int i = 3; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-out" ) == 0 && i + 1 < argc )
            out_arg = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-key" ) == 0 )
            flags |= DEV_IMAGE_SPLIT_KEY_LUMA;
        else
        {
            fprintf( stderr, "image_tool: unknown split option '%s'\n", argv[ i ] );
            return 1;
        }
    }

    if ( cols <= 0 || rows <= 0 )
    {
        fprintf( stderr, "image_tool: grid must be positive integers, got '%s' x '%s'\n",
                 argv[ 1 ], argv[ 2 ] );
        return 1;
    }

    char out_dir[ 512 ];
    if ( out_arg )
        snprintf( out_dir, sizeof( out_dir ), "%s", out_arg );
    else
        default_out_dir( sheet_path, out_dir, (int)sizeof( out_dir ) );

    /* An opaque sheet split without -key produces opaque cells -- almost never what the
       sprite workflow wants.  Say so up front rather than after inspecting the output. */
    if ( !( flags & DEV_IMAGE_SPLIT_KEY_LUMA ) )
    {
        dev_image_t probe;
        if ( dev_image_load( sheet_path, &probe ) )
        {
            if ( !dev_image_has_alpha( &probe ) )
                printf( "note: '%s' has no alpha channel; pass -key to derive transparency"
                        " from luminance\n", sheet_path );
            dev_image_free( &probe );
        }
    }

    int written = dev_image_split_sheet( sheet_path, cols, rows, out_dir, flags );
    if ( written < 0 )
    {
        fprintf( stderr, "image_tool: %s\n", dev_image_last_error() );
        return 1;
    }

    printf( "split '%s' into %d cells -> %s/\n", sheet_path, written, out_dir );
    return 0;
}

/*==============================================================================================
    run_key
==============================================================================================*/

static int
run_key( int argc, char** argv )
{
    if ( argc < 1 )
    {
        fprintf( stderr, "usage: image_tool key <image> [-out <path>] [-pad <px>]\n" );
        return 1;
    }

    const char* in_path = argv[ 0 ];
    const char* out_arg = NULL;
    int         pad     = 0;
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-out" ) == 0 && i + 1 < argc )
            out_arg = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-pad" ) == 0 && i + 1 < argc )
            pad = atoi( argv[ ++i ] );
        else
        {
            fprintf( stderr, "image_tool: unknown key option '%s'\n", argv[ i ] );
            return 1;
        }
    }
    if ( pad < 0 )
    {
        fprintf( stderr, "image_tool: -pad must not be negative, got '%d'\n", pad );
        return 1;
    }

    dev_image_t img;
    if ( !dev_image_load( in_path, &img ) )
    {
        fprintf( stderr, "image_tool: %s\n", dev_image_last_error() );
        return 1;
    }

    if ( dev_image_has_alpha( &img ) )
        printf( "note: '%s' already carries transparency -- keying will still run, but this "
                "may not be the flat-background art the verb is for\n", in_path );

    dev_image_key_luma( &img );

    dev_image_t final_img = img;
    dev_image_t padded    = { 0 };
    bool        did_pad   = false;
    if ( pad > 0 )
    {
        if ( !dev_image_pad( &img, pad, &padded ) )
        {
            fprintf( stderr, "image_tool: %s\n", dev_image_last_error() );
            dev_image_free( &img );
            return 1;
        }
        final_img = padded;
        did_pad   = true;
    }

    char out_path[ 576 ];
    if ( out_arg )
        snprintf( out_path, sizeof( out_path ), "%s", out_arg );
    else
    {
        char stem[ 512 ];
        default_out_dir( in_path, stem, (int)sizeof( stem ) );   // strips the extension
        snprintf( out_path, sizeof( out_path ), "%s_keyed.png", stem );
    }

    bool ok = dev_image_write_png( out_path, &final_img );
    if ( did_pad ) dev_image_free( &padded );
    dev_image_free( &img );
    if ( !ok )
    {
        fprintf( stderr, "image_tool: %s\n", dev_image_last_error() );
        return 1;
    }

    printf( "keyed '%s' -> %s (%dx%d%s)\n", in_path, out_path, final_img.w, final_img.h,
            did_pad ? ", padded" : "" );
    return 0;
}

/*==============================================================================================
    run_svg
==============================================================================================*/

/* Rasterize one SVG file to a transparent PNG.  Prints its own errors; returns false on any
   failure so batch callers can count and continue. */
static bool
svg_bake_one( const char* in_path, const char* out_path, int size, int margin )
{
    dev_vector_svg_t svg;
    if ( !dev_vector_svg_load( in_path, &svg ) )
    {
        fprintf( stderr, "image_tool: %s\n", dev_vector_last_error() );
        return false;
    }

    dev_vector_raster_t raster;
    bool ok = dev_vector_svg_raster( &svg, size, margin, &raster );
    dev_vector_svg_free( &svg );
    if ( !ok )
    {
        fprintf( stderr, "image_tool: %s\n", dev_vector_last_error() );
        return false;
    }

    /* dev_vector_raster_t and dev_image_t share the RGBA8 pixel contract, so the PNG writer
       takes the raster's fields directly. */
    dev_image_t img = { raster.pixels, raster.w, raster.h };
    int         w   = raster.w;
    int         h   = raster.h;
    ok = dev_image_write_png( out_path, &img );
    dev_vector_raster_free( &raster );
    if ( !ok )
    {
        fprintf( stderr, "image_tool: %s\n", dev_image_last_error() );
        return false;
    }

    printf( "svg '%s' -> %s (%dx%d)\n", in_path, out_path, w, h );
    return true;
}

/* Batch state threaded through the sys_file_glob callback. */
typedef struct
{
    const char* out_dir;
    int         size, margin;
    int         failed;

} svg_batch_t;

static bool
svg_glob_cb( const char* filename, const char* full_path, void* userdata )
{
    svg_batch_t* batch = (svg_batch_t*)userdata;

    char stem[ 512 ];
    default_out_dir( filename, stem, (int)sizeof( stem ) );   // strips the extension

    char out_path[ 576 ];
    snprintf( out_path, sizeof( out_path ), "%s/%s.png", batch->out_dir, stem );

    if ( !svg_bake_one( full_path, out_path, batch->size, batch->margin ) )
        ++batch->failed;
    return true;   // keep iterating; failures are counted, not fatal
}

static int
run_svg( int argc, char** argv )
{
    if ( argc < 1 )
    {
        fprintf( stderr, "usage: image_tool svg <input.svg | dir> [-size <px>] [-margin <px>]"
                         " [-out <path | dir>]\n" );
        return 1;
    }

    const char* in_path = argv[ 0 ];
    const char* out_arg = NULL;
    int         size    = 32;
    int         margin  = 0;
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-out" ) == 0 && i + 1 < argc )
            out_arg = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-size" ) == 0 && i + 1 < argc )
            size = atoi( argv[ ++i ] );
        else if ( strcmp( argv[ i ], "-margin" ) == 0 && i + 1 < argc )
            margin = atoi( argv[ ++i ] );
        else
        {
            fprintf( stderr, "image_tool: unknown svg option '%s'\n", argv[ i ] );
            return 1;
        }
    }
    if ( size <= 0 || margin < 0 || size - margin * 2 <= 0 )
    {
        fprintf( stderr, "image_tool: -size %d with -margin %d leaves no room for the art\n",
                 size, margin );
        return 1;
    }

    /* A single existing file rasterizes to one PNG; anything else is treated as a directory
       and every *.svg inside it is rasterized. */
    if ( sys_file_exists( in_path ) )
    {
        char out_path[ 576 ];
        if ( out_arg )
            snprintf( out_path, sizeof( out_path ), "%s", out_arg );
        else
        {
            char stem[ 512 ];
            default_out_dir( in_path, stem, (int)sizeof( stem ) );   // strips the extension
            snprintf( out_path, sizeof( out_path ), "%s.png", stem );
        }
        return svg_bake_one( in_path, out_path, size, margin ) ? 0 : 1;
    }

    svg_batch_t batch = { 0 };
    batch.out_dir     = out_arg ? out_arg : in_path;
    batch.size        = size;
    batch.margin      = margin;

    if ( !sys_dir_make( batch.out_dir ) )
    {
        fprintf( stderr, "image_tool: cannot create output directory '%s'\n", batch.out_dir );
        return 1;
    }

    int found = sys_file_glob( in_path, "*.svg", svg_glob_cb, &batch );
    if ( found == 0 )
    {
        fprintf( stderr, "image_tool: no .svg files found in '%s' (and it is not a file)\n",
                 in_path );
        return 1;
    }

    printf( "rasterized %d of %d svg(s) -> %s/\n", found - batch.failed, found, batch.out_dir );
    return batch.failed ? 1 : 0;
}

/*==============================================================================================
    run_icons -- batch-bake config/icons.manifest.

    Reuses svg_bake_one -- the manifest just names icons (the gui registry key) and their source
    SVGs instead of files, and defaults -size/-margin for SDF headroom instead of svg's flat-icon
    32/0, since the built-in set loads as distance fields by default (icon_load_builtins).
==============================================================================================*/

/* Split one already-trimmed manifest line into whitespace-separated tokens in place (NULs
   inserted at boundaries).  No quoting -- icon names and source filenames do not need spaces. */
static int
icons_tokenize( char* line, char** tok, int max_tok )
{
    int n = 0;
    for ( char* p = line; *p && n < max_tok; )
    {
        while ( *p == ' ' || *p == '\t' ) ++p;
        if ( !*p ) break;
        tok[ n++ ] = p;
        while ( *p && *p != ' ' && *p != '\t' ) ++p;
        if ( *p ) *p++ = '\0';
    }
    return n;
}

/* Bake one manifest line.  Prints its own errors (via svg_bake_one or its own option parsing);
   returns false so the caller can count failures and keep going. */
static bool
icons_bake_line( char* line )
{
    char* tok[ 8 ];
    int   n = icons_tokenize( line, tok, 8 );
    if ( n < 2 )
    {
        fprintf( stderr, "image_tool: icons.manifest: bad line (need <name> <source.svg>): %s\n",
                 line );
        return false;
    }

    const char* name = tok[ 0 ];
    char        in_path[ 384 ];
    snprintf( in_path, sizeof( in_path ), "assets/ui/icon/%s", tok[ 1 ] );

    int  size   = 256;   // generous headroom over icon_register_sdf's out_max default of 62
    int  margin = 16;    // an SDF bake needs an outside to fall off into
    char out_path[ 384 ];
    snprintf( out_path, sizeof( out_path ), "content/ui/icon/%s.png", name );

    for ( int i = 2; i < n; ++i )
    {
        if ( strcmp( tok[ i ], "-size" ) == 0 && i + 1 < n )
            size = atoi( tok[ ++i ] );
        else if ( strcmp( tok[ i ], "-margin" ) == 0 && i + 1 < n )
            margin = atoi( tok[ ++i ] );
        else if ( strcmp( tok[ i ], "-out" ) == 0 && i + 1 < n )
            snprintf( out_path, sizeof( out_path ), "%s", tok[ ++i ] );
        else
        {
            fprintf( stderr, "image_tool: icons.manifest: unknown option '%s' (icon '%s')\n",
                     tok[ i ], name );
            return false;
        }
    }

    if ( size <= 0 || margin < 0 || size - margin * 2 <= 0 )
    {
        fprintf( stderr, "image_tool: icons.manifest: -size %d with -margin %d leaves no room"
                          " for the art (icon '%s')\n", size, margin, name );
        return false;
    }

    return svg_bake_one( in_path, out_path, size, margin );
}

static int
run_icons( int argc, char** argv )
{
    if ( argc < 1 )
    {
        fprintf( stderr, "usage: image_tool icons <manifest>\n" );
        return 1;
    }

    FILE* f = fopen( argv[ 0 ], "rb" );
    if ( !f )
    {
        fprintf( stderr, "image_tool: cannot open manifest '%s'\n", argv[ 0 ] );
        return 1;
    }

    char line[ 512 ];
    int  total = 0, baked = 0;

    while ( fgets( line, sizeof( line ), f ) )
    {
        char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;
        if ( *p == '#' || *p == '\0' || *p == '\r' || *p == '\n' )
            continue;

        size_t len = strlen( p );
        while ( len && ( p[ len - 1 ] == '\n' || p[ len - 1 ] == '\r'
                      || p[ len - 1 ] == ' '  || p[ len - 1 ] == '\t' ) )
            p[ --len ] = '\0';
        if ( !*p )
            continue;

        ++total;
        if ( icons_bake_line( p ) )
            ++baked;
    }
    fclose( f );

    printf( "rasterized %d of %d icon(s) from '%s'\n", baked, total, argv[ 0 ] );
    return baked == total ? 0 : 1;
}

/*==============================================================================================
    run_info
==============================================================================================*/

static int
run_info( int argc, char** argv )
{
    if ( argc < 1 )
    {
        fprintf( stderr, "usage: image_tool info <image>...\n" );
        return 1;
    }

    int failures = 0;
    for ( int i = 0; i < argc; ++i )
    {
        dev_image_t img;
        if ( !dev_image_load( argv[ i ], &img ) )
        {
            fprintf( stderr, "image_tool: %s\n", dev_image_last_error() );
            ++failures;
            continue;
        }

        printf( "%s: %dx%d, transparency: %s\n",
                argv[ i ], img.w, img.h, dev_image_has_alpha( &img ) ? "yes" : "no" );
        dev_image_free( &img );
    }
    return failures ? 1 : 0;
}

/*==============================================================================================
    main
==============================================================================================*/

static void
print_usage( void )
{
    fprintf( stderr,
             "usage: image_tool split <sheet> <cols> <rows> [-out <dir>] [-key]\n"
             "       image_tool key   <image> [-out <path>] [-pad <px>]\n"
             "       image_tool svg   <input.svg | dir> [-size <px>] [-margin <px>] [-out <path | dir>]\n"
             "       image_tool icons <manifest>\n"
             "       image_tool info  <image>...\n"
             "\n"
             "  split  cut a sprite sheet into cols x rows individual .png files\n"
             "         -out <dir>  output directory (default: sheet path minus extension)\n"
             "         -key        alpha = max(r,g,b); makes a dark background transparent\n"
             "  key    alpha = max(r,g,b) on a single image; -pad adds a transparent margin\n"
             "         -out <path> output file (default: <image>_keyed.png)\n"
             "         -pad <px>   transparent margin added after keying (an SDF bake needs one)\n"
             "  svg    rasterize an SVG file (or every .svg in a directory) to transparent PNG\n"
             "         -size <px>    longest output edge, margins included (default: 32)\n"
             "         -margin <px>  transparent border per side (an SDF bake needs one)\n"
             "         -out <path | dir>  output file, or directory for a directory input\n"
             "  icons  batch-bake config/icons.manifest -- see the file header for the format\n"
             "  info   print image dimensions and whether transparency is present\n" );
}

int
main( int argc, char** argv )
{
    if ( argc >= 2 && strcmp( argv[ 1 ], "split" ) == 0 )
        return run_split( argc - 2, argv + 2 );
    if ( argc >= 2 && strcmp( argv[ 1 ], "key" ) == 0 )
        return run_key( argc - 2, argv + 2 );
    if ( argc >= 2 && strcmp( argv[ 1 ], "svg" ) == 0 )
        return run_svg( argc - 2, argv + 2 );
    if ( argc >= 2 && strcmp( argv[ 1 ], "icons" ) == 0 )
        return run_icons( argc - 2, argv + 2 );
    if ( argc >= 2 && strcmp( argv[ 1 ], "info" ) == 0 )
        return run_info( argc - 2, argv + 2 );

    print_usage();
    return 1;
}

// clang-format on
/*============================================================================================*/
