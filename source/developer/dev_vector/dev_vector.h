#ifndef DEV_VECTOR_H
#define DEV_VECTOR_H
/*==============================================================================================

    dev_vector.h -- Developer vector graphics (SVG) utility library.

    Shared implementation for offline / development vector work: parse an SVG file and
    rasterize it to an RGBA8 bitmap at any pixel size.  Multiple clients link this statically
    -- image_tool (SVG-to-PNG icon baking) today, in-engine developer tooling later -- so the
    vector work lives here and clients stay thin front-ends.

    Icon baking (the founding use case): gui icons authored as SVG have no background to key
    out -- a path is either filled or it is not -- so rasterizing them produces clean alpha
    coverage directly, sidestepping the luma-key workflow raster sheets need.  Rasterized
    small (e.g. 32 px) the output feeds the gui coverage icon path as-is; rasterized large
    with a transparent margin (e.g. 1024 px, 64 px margin) it is a valid source for an SDF
    bake, which needs an outside to fall off into (gui_sdf_bake.c).

    Usage
    -----
        dev_vector_svg_t svg;
        if ( !dev_vector_svg_load( "gear.svg", &svg ) )
            printf( "%s\n", dev_vector_last_error() );

        dev_vector_raster_t r;
        if ( dev_vector_svg_raster( &svg, 32, 0, &r ) )
        {
            // r.pixels is RGBA8, r.w x r.h, non-premultiplied alpha
            dev_vector_raster_free( &r );
        }
        dev_vector_svg_free( &svg );

    Parsing and rasterization use nanosvg (vendor/nanosvg.h, vendor/nanosvgrast.h), compiled
    TU-locally inside dev_vector.c.

==============================================================================================*/

#include "orb.h"

/* A parsed SVG.  nsvg is the nanosvg image, opaque outside dev_vector.c.  Release with
   dev_vector_svg_free(). */
typedef struct
{
    void* nsvg;
    f32   w, h;   // authored viewport size in svg units

} dev_vector_svg_t;

/* A rasterized bitmap.  Always RGBA8, non-premultiplied: w * h * 4 bytes, row-major,
   malloc'd.  Same field layout as dev_image_t so clients can hand pixels between the two
   libraries.  Release with dev_vector_raster_free(). */
typedef struct
{
    u8* pixels;
    int w, h;

} dev_vector_raster_t;

/* Parse an SVG file.  Returns false (and sets the error string) if the file is missing,
   unparseable, or has a degenerate viewport; *out is zeroed on failure. */
bool        dev_vector_svg_load( const char* path, dev_vector_svg_t* out );
void        dev_vector_svg_free( dev_vector_svg_t* svg );

/* Rasterize onto a fully transparent canvas.  The art is scaled uniformly so its longest
   edge fits size_px - 2 * margin_px, then surrounded by margin_px of transparency on every
   side; output dimensions are the scaled art plus margins (a square viewport yields exactly
   size_px x size_px).  An SDF bake needs the margin (gui_sdf_bake.c); plain coverage icons
   can pass 0.  The caller frees out with dev_vector_raster_free(). */
bool        dev_vector_svg_raster( const dev_vector_svg_t* svg, int size_px, int margin_px,
                                   dev_vector_raster_t* out );
void        dev_vector_raster_free( dev_vector_raster_t* r );

const char* dev_vector_last_error( void );

/*============================================================================================*/
#endif /* DEV_VECTOR_H */
