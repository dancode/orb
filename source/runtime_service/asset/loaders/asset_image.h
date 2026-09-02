#ifndef ASSET_IMAGE_H
#define ASSET_IMAGE_H
/*==============================================================================================

    runtime_service/asset/loaders/asset_image.h -- built-in image asset resource.

    The asset service auto-registers an "image" type (ASSET_TYPE_IMAGE, see asset_api.c) that
    accepts the formats below.  acquire()ing a resource as that type finds its file (cooked
    .tex first, else a source image), decodes if needed, and uploads it to a bindless RHI
    texture; get() then returns a pointer to this struct.  Include this header in the code that
    DRAWS an acquired image so it can read the bindless slot and dimensions:

        aid_t          id = asset()->acquire( RID( "data/sprite" ), ASSET_TYPE_IMAGE );
        asset_image_t* im = asset()->get( id );
        if ( im )
            draw()->image( cx, cy, (f32)im->width, (f32)im->height,
                           im->tex_index, draw()->sampler_linear(), tint );

    Only bytes + dimensions are exposed; the backing rhi_texture_t handle stays private to the
    loader, which releases it on unload.

==============================================================================================*/

#include "orb.h"

/* Extensions the built-in image type accepts, in preference order.  ".tex" is the cooked
   format (asset_tex.h), uploaded with zero decode, so a cooked mirror or a shipped pack wins
   over the source beside it; the source formats are decoded live via stb_image.  The loader
   also sniffs the .tex magic regardless of extension, so a cooked file loads correctly even
   if it still carries a source extension. */
#define ASSET_IMAGE_EXTS \
    { ".tex", ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".gif", ".hdr" }

typedef struct asset_image_s
{
    u32 tex_index;   // bindless texture slot -> draw()->image( ..., tex_index, ... )
    u32 width;       // decoded pixel width
    u32 height;      // decoded pixel height

} asset_image_t;

/*============================================================================================*/
#endif    // ASSET_IMAGE_H
