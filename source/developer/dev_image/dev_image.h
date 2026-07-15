#ifndef DEV_IMAGE_H
#define DEV_IMAGE_H
/*==============================================================================================

    dev_image.h -- Developer 2D image utility library.

    Shared implementation for offline / development image work: decode an image file to RGBA8,
    crop regions, derive alpha coverage, and write PNGs back out.  Multiple clients link this
    statically -- image_tool (CLI sprite-sheet splitter) today, in-engine developer tooling
    later -- so the pixel work lives here and clients stay thin front-ends.

    All images are RGBA8 in memory (stb_image decodes with req_comp = 4), so transparency is
    uniform regardless of how the source spelled it: a real alpha channel, or an RGB/paletted
    image with a tRNS color-key, both land in the decoded alpha channel.

    Sprite-sheet splitting (the founding use case): AI image generators emit grids of icons on
    an opaque dark background.  dev_image_split_sheet() cuts such a sheet into per-cell PNGs;
    the DEV_IMAGE_SPLIT_KEY_LUMA flag derives alpha from luminance so the dark background
    becomes transparent, matching the gui icon loader's coverage convention (gui_icon_load.c).

    Usage
    -----
        int n = dev_image_split_sheet( "sheet.png", 4, 2, "out_dir", DEV_IMAGE_SPLIT_KEY_LUMA );
        if ( n < 0 )
            printf( "%s\n", dev_image_last_error() );

    Reading uses stb_image (all its formats: PNG, JPG, BMP, TGA, GIF, ...); writing is PNG only
    via stb_image_write.  Both are compiled TU-locally inside dev_image.c (STATIC), so linking
    dev_image never collides with the gui backend's or asset service's own stb copies.

==============================================================================================*/

#include "orb.h"

/* A decoded image.  Always RGBA8: w * h * 4 bytes, row-major, malloc'd.
   Release with dev_image_free(). */
typedef struct
{
    u8* pixels;
    int w, h;

} dev_image_t;

/* Decode an image file into RGBA8.  Returns false (and sets the error string) if the file is
   missing or undecodable; *out is zeroed on failure. */
bool        dev_image_load( const char* path, dev_image_t* out );
void        dev_image_free( dev_image_t* img );

/* Write an RGBA8 image as a PNG.  The parent directory must already exist. */
bool        dev_image_write_png( const char* path, const dev_image_t* img );

/* Copy the rect (x, y, w, h) of src into a freshly allocated image.  Fails if the rect falls
   outside src.  The caller frees out with dev_image_free(). */
bool        dev_image_crop( const dev_image_t* src, int x, int y, int w, int h,
                            dev_image_t* out );

/* True if any pixel's alpha differs from 255 -- i.e. the image carries real transparency. */
bool        dev_image_has_alpha( const dev_image_t* img );

/* Derive alpha from luminance: alpha = max( r, g, b ) per pixel.  Knocks an opaque dark
   background out to transparency (the AI-generator sheet case); colour channels are left
   untouched.  Same coverage convention as the gui icon loader. */
void        dev_image_key_luma( dev_image_t* img );

/* dev_image_split_sheet flags */
#define DEV_IMAGE_SPLIT_KEY_LUMA  ( 1u << 0 )   /* run dev_image_key_luma before cutting */

/* Cut a sprite sheet into cols * rows equal cells and write each as
   <out_dir>/<sheet stem>_r<row>_c<col>.png.  The sheet dimensions must divide evenly by the
   grid -- a remainder means the grid count is wrong, and silently truncating would produce
   misaligned crops, so it is an error instead.  out_dir is created if missing.
   Returns the number of cells written, or -1 on error (see dev_image_last_error()). */
int         dev_image_split_sheet( const char* sheet_path, int cols, int rows,
                                   const char* out_dir, u32 flags );

const char* dev_image_last_error( void );

/*============================================================================================*/
#endif /* DEV_IMAGE_H */
