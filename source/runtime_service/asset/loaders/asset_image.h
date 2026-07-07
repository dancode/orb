#ifndef ASSET_IMAGE_H
#define ASSET_IMAGE_H
/*==============================================================================================

    runtime_service/asset/loaders/asset_image.h -- built-in image asset resource.

    The asset service auto-registers an "image" type (see asset_api.c) for the common source
    formats below.  acquire()ing one of those files decodes it (stb_image) and uploads it to a
    bindless RHI texture; get() then returns a pointer to this struct.  Include this header in
    the code that DRAWS an acquired image so it can read the bindless slot and dimensions:

        asset_id_t   id  = asset()->acquire( "data/sprite.png" );
        asset_image_t* im = asset()->get( id );
        if ( im )
            draw()->image( cx, cy, (f32)im->width, (f32)im->height,
                           im->tex_index, draw()->sampler_linear(), tint );

    Only bytes + dimensions are exposed; the backing rhi_texture_t handle stays private to the
    loader, which releases it on unload.

==============================================================================================*/

#include "orb.h"

/* Source formats claimed by the built-in image type (decoded via stb_image). */
#define ASSET_IMAGE_EXTS { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".gif", ".hdr" }

typedef struct asset_image_s
{
    u32 tex_index;   // bindless texture slot -> draw()->image( ..., tex_index, ... )
    u32 width;       // decoded pixel width
    u32 height;      // decoded pixel height

} asset_image_t;

/*============================================================================================*/
#endif    // ASSET_IMAGE_H
