/*==============================================================================================

    runtime_service/asset/loaders/asset_image.c -- built-in image loader (direct path).

    Decodes a source image (PNG/JPG/... via stb_image) from the bytes fs handed us, forces it
    to RGBA8, and uploads it to a GPU-only, bindless RHI texture.  The asset resource is this
    file's private image_res_t whose FIRST member is the public asset_image_t, so the pointer
    the registry stores doubles as the asset_image_t* returned by get().

    Direct-only for now (Phase 3): it always decodes the authoring format live.  The cooked
    path (a versioned .tex uploaded with zero transform) slots in here later behind an
    extension/header check -- see the loader design in ASSET_SYSTEM_PLAN.md.

    Compiled ONLY in the asset service TU (stb's implementation lives here, never in core).

==============================================================================================*/

#include <stdlib.h>

#include "runtime_service/asset/loaders/asset_image.h"
#include "runtime_service/rhi/rhi_api.h"

/* stb_image: memory-only decode (no fopen path); implementation compiled right here. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "vendor/stb_image.h"

/*==============================================================================================
    Private resource -- public fields first so (asset_image_t*)resource is valid.
==============================================================================================*/

typedef struct image_res_s
{
    asset_image_t pub;   // MUST be first: get() returns this pointer as asset_image_t*
    rhi_texture_t tex;   // backing handle, kept only to destroy on unload

} image_res_t;

/*==============================================================================================
    Loader
==============================================================================================*/

static void*
asset_image_load( const char* vpath, const void* data, u32 size, void* userdata )
{
    UNUSED( userdata );

    /* Decode to RGBA8 regardless of the source channel count. */
    int      w = 0, h = 0, comp = 0;
    stbi_uc* pixels =
        stbi_load_from_memory( ( const stbi_uc* )data, ( int )size, &w, &h, &comp, STBI_rgb_alpha );
    if ( !pixels )
    {
        LOG_ERROR( "asset: image decode failed for '%s': %s", vpath, stbi_failure_reason() );
        return NULL;
    }

    /* GPU-only texture, filled through the staged upload path. */
    rhi_texture_t tex = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = ( u32 )w,
        .height       = ( u32 )h,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_RGBA8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = vpath,
    } );
    if ( !rhi_handle_valid( tex ) )
    {
        LOG_ERROR( "asset: texture_create failed for '%s' (%dx%d)", vpath, w, h );
        stbi_image_free( pixels );
        return NULL;
    }

    if ( !rhi()->upload_texture( tex, pixels, ( u32 )( w * h * 4 ), 0, 0 ) )
    {
        LOG_ERROR( "asset: texture upload failed for '%s'", vpath );
        rhi()->texture_destroy( tex );
        stbi_image_free( pixels );
        return NULL;
    }
    stbi_image_free( pixels );

    /* Persistent bindless slot so draw()->image can sample without a per-draw descriptor. */
    u32 idx = rhi()->register_texture( tex );
    if ( idx == 0 )
    {
        LOG_ERROR( "asset: bindless register failed for '%s'", vpath );
        rhi()->texture_destroy( tex );
        return NULL;
    }

    image_res_t* res = ( image_res_t* )malloc( sizeof( image_res_t ) );
    if ( !res )
    {
        rhi()->unregister_texture( idx );
        rhi()->texture_destroy( tex );
        return NULL;
    }
    res->pub.tex_index = idx;
    res->pub.width     = ( u32 )w;
    res->pub.height    = ( u32 )h;
    res->tex           = tex;
    return res;
}

static void
asset_image_unload( void* resource, void* userdata )
{
    UNUSED( userdata );

    image_res_t* res = ( image_res_t* )resource;
    if ( !res )
        return;

    rhi()->unregister_texture( res->pub.tex_index );
    rhi()->texture_destroy( res->tex );
    free( res );
}

/*============================================================================================*/
