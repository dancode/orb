/*==============================================================================================

    dev_vector.c -- Developer vector graphics (SVG) utility library.

    Unity build entry for the dev_vector static library.  Compiles nanosvg (parse) and
    nanosvgrast (rasterize) here.  nanosvg has no _STATIC option (unlike stb_image), so its
    implementation symbols are external -- this TU must remain the engine's only site that
    defines NANOSVG_IMPLEMENTATION / NANOSVGRAST_IMPLEMENTATION, or a host linking two copies
    would collide.  Header-only inclusion of vendor/nanosvg.h elsewhere is fine.

    File reads go through nanosvg's own stdio path (nsvgParseFromFile).  No sys usage; plain
    C stdio keeps this POSIX-clean.

==============================================================================================*/

// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "orb.h"

/* C4702 (unreachable code) is emitted by MSVC's back end after warning-state pops are
   unwound, so PUSH_WARNINGS cannot mask it for the vendored code; disable it TU-wide. */
#if COMPILER_MSVC
    #pragma warning( disable : 4702 )
#endif

PUSH_WARNINGS
#define NANOSVG_IMPLEMENTATION
#include "vendor/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "vendor/nanosvgrast.h"
POP_WARNINGS

#include "developer/dev_vector/dev_vector.h"

/*==============================================================================================
    Error reporting -- one static buffer, printf-style setter, mirrored from dev_image.
==============================================================================================*/

static char g_error[ 512 ];

static bool
err( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( g_error, sizeof( g_error ), fmt, args );
    va_end( args );
    return false;
}

const char*
dev_vector_last_error( void )
{
    return g_error;
}

/*==============================================================================================
    Load / free
==============================================================================================*/

bool
dev_vector_svg_load( const char* path, dev_vector_svg_t* out )
{
    memset( out, 0, sizeof( *out ) );
    if ( !path )
        return err( "dev_vector_svg_load: NULL path" );

    /* "px" units at 96 dpi: the CSS reference pixel, so authored sizes mean what the design
       tool showed.  The unit choice only matters for physical-unit (mm/pt) documents. */
    NSVGimage* image = nsvgParseFromFile( path, "px", 96.0f );
    if ( !image )
        return err( "cannot read or parse '%s'", path );

    if ( image->width <= 0.0f || image->height <= 0.0f )
    {
        nsvgDelete( image );
        return err( "'%s' has a degenerate viewport (%g x %g)",
                    path, image->width, image->height );
    }

    out->nsvg = image;
    out->w    = image->width;
    out->h    = image->height;
    return true;
}

void
dev_vector_svg_free( dev_vector_svg_t* svg )
{
    if ( svg && svg->nsvg )
        nsvgDelete( (NSVGimage*)svg->nsvg );
    if ( svg )
        memset( svg, 0, sizeof( *svg ) );
}

/*==============================================================================================
    Rasterize
==============================================================================================*/

bool
dev_vector_svg_raster( const dev_vector_svg_t* svg, int size_px, int margin_px,
                       dev_vector_raster_t* out )
{
    memset( out, 0, sizeof( *out ) );
    if ( !svg || !svg->nsvg )
        return err( "dev_vector_svg_raster: NULL source" );
    if ( margin_px < 0 )
        return err( "dev_vector_svg_raster: negative margin" );

    int fit = size_px - margin_px * 2;
    if ( fit <= 0 )
        return err( "dev_vector_svg_raster: size %d leaves no room inside margin %d",
                    size_px, margin_px );

    /* Uniform scale: longest viewport edge lands on `fit` pixels; ceil so a fractional short
       edge rounds up rather than clipping the art's last row/column. */
    f32 longest = svg->w > svg->h ? svg->w : svg->h;
    f32 scale   = (f32)fit / longest;
    int w       = (int)ceilf( svg->w * scale ) + margin_px * 2;
    int h       = (int)ceilf( svg->h * scale ) + margin_px * 2;

    /* calloc, not malloc: nsvgRasterize composites shapes over the existing buffer, so the
       canvas must start fully transparent -- and the margin must stay (0,0,0,0). */
    u8* pixels = (u8*)calloc( (size_t)w * (size_t)h, 4 );
    if ( !pixels )
        return err( "dev_vector_svg_raster: out of memory (%dx%d)", w, h );

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if ( !rast )
    {
        free( pixels );
        return err( "dev_vector_svg_raster: cannot create rasterizer" );
    }

    nsvgRasterize( rast, (NSVGimage*)svg->nsvg, (f32)margin_px, (f32)margin_px, scale,
                   pixels, w, h, w * 4 );
    nsvgDeleteRasterizer( rast );

    out->pixels = pixels;
    out->w      = w;
    out->h      = h;
    return true;
}

void
dev_vector_raster_free( dev_vector_raster_t* r )
{
    if ( r && r->pixels )
        free( r->pixels );
    if ( r )
        memset( r, 0, sizeof( *r ) );
}

// clang-format on
/*============================================================================================*/
