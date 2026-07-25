/*==============================================================================================

    runtime_service/gui/render/resource/gui_atlas.c -- Shared GPU-atlas asset (implementation).

    See gui_atlas.h for the rationale.  Three functions, each mirroring the create/upload/
    destroy sequence that used to be duplicated between the font registry and the icon atlas.

    First include in gui_render.c: gui_res_atlas.c builds THE shared atlas on top of these
    three, and every stage after it renders from what that atlas holds.

==============================================================================================*/
// clang-format off

bool
gui_atlas_create( gui_atlas_t* a, u32 w, u32 h, const u8* pixels, const char* debug_name )
{
    *a = ( gui_atlas_t ){ 0 };

    rhi_texture_t tex = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = w,
        .height       = h,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_R8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = debug_name,
    } );
    if ( !rhi_handle_valid( tex ) )
        return false;

    if ( !rhi()->upload_texture( tex, pixels, w * h, 0, 0 ) )
    {
        rhi()->texture_destroy( tex );
        return false;
    }

    u32 idx = rhi()->register_texture( tex );
    if ( idx == 0 )
    {
        rhi()->texture_destroy( tex );
        return false;
    }

    a->atlas     = tex;
    a->atlas_idx = idx;
    a->atlas_w   = w;
    a->atlas_h   = h;
    return true;
}

void
gui_atlas_upload( gui_atlas_t* a, const u8* pixels )
{
    if ( a->atlas_idx == 0 )
        return;
    rhi()->upload_texture( a->atlas, pixels, a->atlas_w * a->atlas_h, 0, 0 );
}

void
gui_atlas_destroy( gui_atlas_t* a )
{
    if ( a->atlas_idx != 0 )
        rhi()->unregister_texture( a->atlas_idx );
    if ( rhi_handle_valid( a->atlas ) )
        rhi()->texture_destroy( a->atlas );
    *a = ( gui_atlas_t ){ 0 };
}

// clang-format on
/*============================================================================================*/
