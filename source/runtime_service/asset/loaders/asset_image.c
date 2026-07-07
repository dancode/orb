/*==============================================================================================

    runtime_service/asset/loaders/asset_image.c -- built-in image loader (direct path).

    Turns the bytes fs handed us into a GPU-only, bindless RHI texture.  Two input paths meet
    at a common upload step:

        source  -- a PNG/JPG/... decoded live to RGBA8 via stb_image (dev iteration).
        cooked  -- a .tex (asset_tex.h): a pre-decoded RGBA8 payload uploaded with ZERO
                   transform.  Detected by the ORBT magic in the header, so it works whether
                   the file ends in .tex or still carries its source extension.

    The asset resource is this file's private image_res_t whose FIRST member is the public
    asset_image_t, so the pointer the registry stores doubles as the asset_image_t* from get().

    Compiled ONLY in the asset service TU (stb's implementation lives here, never in core).

==============================================================================================*/

#include <stdlib.h>

#include "runtime_service/asset/loaders/asset_image.h"
#include "runtime_service/asset/loaders/asset_tex.h"
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
    Upload -- shared tail for both input paths: RGBA8 pixels -> bindless GPU texture -> resource.
==============================================================================================*/

static image_res_t*
image_upload_rgba8( const char* vpath, const void* pixels, u32 w, u32 h )
{
    /* GPU-only texture, filled through the staged upload path. */
    rhi_texture_t tex = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = w,
        .height       = h,
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
        LOG_ERROR( "asset: texture_create failed for '%s' (%ux%u)", vpath, w, h );
        return NULL;
    }

    if ( !rhi()->upload_texture( tex, pixels, w * h * 4, 0, 0 ) )
    {
        LOG_ERROR( "asset: texture upload failed for '%s'", vpath );
        rhi()->texture_destroy( tex );
        return NULL;
    }

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
    res->pub.width     = w;
    res->pub.height    = h;
    res->tex           = tex;
    return res;
}

/*==============================================================================================
    Input paths
==============================================================================================*/

/* Cooked: the payload is already RGBA8; upload it verbatim. Called only after the magic matched. */
static void*
image_load_cooked( const char* vpath, const void* data, u32 size )
{
    const asset_tex_header_t* hdr = ( const asset_tex_header_t* )data;

    if ( hdr->version != ASSET_TEX_VERSION )
    {
        LOG_ERROR( "asset: .tex '%s' version %u != %u", vpath, hdr->version, ASSET_TEX_VERSION );
        return NULL;
    }
    if ( hdr->format != ASSET_TEX_FORMAT_RGBA8 )
    {
        LOG_ERROR( "asset: .tex '%s' unsupported format %u", vpath, hdr->format );
        return NULL;
    }

    u64 need = ( u64 )sizeof( asset_tex_header_t ) + hdr->data_size;
    if ( hdr->data_size != hdr->width * hdr->height * 4 || need > size )
    {
        LOG_ERROR( "asset: .tex '%s' truncated/inconsistent (%ux%u, %u payload, %u file)", vpath,
                   hdr->width, hdr->height, hdr->data_size, size );
        return NULL;
    }

    const void* pixels = ( const u8* )data + sizeof( asset_tex_header_t );
    return image_upload_rgba8( vpath, pixels, hdr->width, hdr->height );
}

/* Source: decode the authoring format to RGBA8 with stb_image, then upload. */
static void*
image_load_source( const char* vpath, const void* data, u32 size )
{
    int      w = 0, h = 0, comp = 0;
    stbi_uc* pixels =
        stbi_load_from_memory( ( const stbi_uc* )data, ( int )size, &w, &h, &comp, STBI_rgb_alpha );
    if ( !pixels )
    {
        LOG_ERROR( "asset: image decode failed for '%s': %s", vpath, stbi_failure_reason() );
        return NULL;
    }

    image_res_t* res = image_upload_rgba8( vpath, pixels, ( u32 )w, ( u32 )h );
    stbi_image_free( pixels );
    return res;
}

/*==============================================================================================
    Loader entry -- sniff the cooked magic, else decode as a source image.
==============================================================================================*/

static void*
asset_image_load( const char* vpath, const void* data, u32 size, void* userdata )
{
    UNUSED( userdata );

    if ( size >= sizeof( asset_tex_header_t ) &&
         ( ( const asset_tex_header_t* )data )->magic == ASSET_TEX_MAGIC )
        return image_load_cooked( vpath, data, size );

    return image_load_source( vpath, data, size );
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
