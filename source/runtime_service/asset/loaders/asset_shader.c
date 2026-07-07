/*==============================================================================================

    runtime_service/asset/loaders/asset_shader.c -- built-in shader loader (.oshd).

    Thin adapter between the asset service's bytes-in contract and the RHI's .oshd parser:
    the container validation, bindless-contract enforcement, and reflection capture all live
    in rhi()->shader_load_oshd_memory (vk_shader_load.c).  This file only peeks at the header
    for the fields the resource exposes (stage / pc_size / layout_hash) and owns the
    rhi_shader_t lifetime.

    The resource is a private shader_res_t whose FIRST member is the public asset_shader_t,
    so the pointer the registry stores doubles as the asset_shader_t* from get() (the
    image_res_t pattern).

==============================================================================================*/

#include <stdlib.h>

#include "runtime_service/asset/loaders/asset_shader.h"
#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/rhi/rhi_shader_format.h"

typedef struct shader_res_s
{
    asset_shader_t pub;   // MUST be first: get() returns this pointer as asset_shader_t*

} shader_res_t;

static void*
asset_shader_load( const char* vpath, const void* data, u32 size, void* userdata )
{
    UNUSED( userdata );

    /* The RHI parser fully validates the container; it just needs at least a header to look
       at, and we read the same header afterwards for the public fields. */
    if ( size < sizeof( oshd_header_t ) )
    {
        LOG_ERROR( "asset: '%s' too small for an .oshd container (%u bytes)", vpath, size );
        return NULL;
    }

    rhi_shader_t shader = rhi()->shader_load_oshd_memory( data, size, vpath );
    if ( !rhi_handle_valid( shader ) )
        return NULL;    /* the parser already logged the specific failure */

    shader_res_t* res = ( shader_res_t* )malloc( sizeof( shader_res_t ) );
    if ( !res )
    {
        rhi()->shader_destroy( shader );
        return NULL;
    }

    const oshd_header_t* h = ( const oshd_header_t* )data;
    res->pub.shader        = shader;
    res->pub.stage         = h->stage;
    res->pub.pc_size       = h->pc_size;
    res->pub.layout_hash   = h->layout_hash;
    return res;
}

static void
asset_shader_unload( void* resource, void* userdata )
{
    UNUSED( userdata );

    shader_res_t* res = ( shader_res_t* )resource;
    if ( !res )
        return;

    rhi()->shader_destroy( res->pub.shader );
    free( res );
}

/*============================================================================================*/
