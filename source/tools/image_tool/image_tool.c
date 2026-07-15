/*==============================================================================================

    tools/image_tool/image_tool.c -- Offline 2D image utility.

    Thin CLI front-end over the dev_image library (developer/dev_image).  The founding job is
    cutting AI-generated sprite sheets into individual transparent PNGs so single images can be
    picked out selectively instead of cropped by hand; further image verbs land here as needed.

    Usage:
        image_tool split <sheet> <cols> <rows> [-out <dir>] [-key]
        image_tool info  <image>...

    split -- cut <sheet> into cols * rows equal cells and write each cell as
             <dir>/<stem>_r<row>_c<col>.png.  The sheet must divide evenly by the grid.
             -out <dir>   output directory (default: <sheet path minus extension>/)
             -key         derive alpha from luminance (alpha = max rgb), knocking an opaque
                          dark background out to transparency.  Suggested automatically when
                          the sheet carries no alpha of its own.

    info  -- print dimensions, grid hints, and whether the image carries transparency.

    Input is any stb_image format (PNG, JPG, BMP, TGA, GIF, ...); output is always PNG.

    Link deps: dev_image (pixel work), sys (directory creation inside dev_image)

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "developer/dev_image/dev_image.h"

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
             "       image_tool info  <image>...\n"
             "\n"
             "  split  cut a sprite sheet into cols x rows individual .png files\n"
             "         -out <dir>  output directory (default: sheet path minus extension)\n"
             "         -key        alpha = max(r,g,b); makes a dark background transparent\n"
             "  info   print image dimensions and whether transparency is present\n" );
}

int
main( int argc, char** argv )
{
    if ( argc >= 2 && strcmp( argv[ 1 ], "split" ) == 0 )
        return run_split( argc - 2, argv + 2 );
    if ( argc >= 2 && strcmp( argv[ 1 ], "info" ) == 0 )
        return run_info( argc - 2, argv + 2 );

    print_usage();
    return 1;
}

// clang-format on
/*============================================================================================*/
