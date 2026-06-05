/*==============================================================================================

    vulkan/vk_texture.c -- VkImage + VkImageView + VkSampler lifecycle.

    Slot pool: vk.textures[ VK_MAX_TEXTURES ] (vk_texture_slot_t).
    Each texture gets a default "full-range" image view covering all mips and layers.
    Sampler objects share the file for filter/address locality but use a separate pool.

    Mip level auto-compute: if desc->mip_levels == 0, floor(log2(max(w,h))) + 1.

    Format and usage conversions live in vk_convert.c.

==============================================================================================*/

/*==============================================================================================
    Slot allocation helpers
==============================================================================================*/

static i32
vk_texture_alloc_slot( void )
{
    for ( u32 i = 1; i < VK_MAX_TEXTURES; ++i )
    {
        if ( vk.textures[ i ].image == VK_NULL_HANDLE )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_texture_validate( rhi_texture_t handle )
{
    return handle.id > 0 && handle.id < VK_MAX_TEXTURES
        && vk.textures[ handle.id ].image != VK_NULL_HANDLE;
}

/*==============================================================================================
    Texture creation / destruction
==============================================================================================*/

static rhi_texture_t
vk_texture_create( const rhi_texture_desc_t* desc )
{
    if ( !desc || desc->width == 0 || desc->height == 0 )
        return ( rhi_texture_t ){ RHI_NULL_HANDLE };

    i32 idx = vk_texture_alloc_slot();
    if ( idx < 0 )
    {
        LOG_ERROR( "texture pool exhausted (VK_MAX_TEXTURES = %d)", VK_MAX_TEXTURES );
        return ( rhi_texture_t ){ RHI_NULL_HANDLE };
    }

    vk_texture_slot_t* slot = &vk.textures[ (u32)idx ];

    u16 mips = desc->mip_levels;
    if ( mips == 0 )
    {
        u32 dim = desc->width > desc->height ? desc->width : desc->height;
        if ( desc->depth > dim ) dim = desc->depth;
        mips = 1;
        while ( dim > 1 ) { dim >>= 1; mips++; }
    }

    u16 depth        = desc->depth        > 1 ? desc->depth        : 1;
    u16 array_layers = desc->array_layers > 1 ? desc->array_layers : 1;
    VkFormat vk_fmt  = rhi_format_to_vk( desc->format );

    VkImageCreateInfo img_ci      = { 0 };
    img_ci.sType                  = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType              = ( depth > 1 ) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    img_ci.format                 = vk_fmt;
    img_ci.extent.width           = desc->width;
    img_ci.extent.height          = desc->height;
    img_ci.extent.depth           = depth;
    img_ci.mipLevels              = mips;
    img_ci.arrayLayers            = array_layers;
    img_ci.samples                = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling                 = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage                  = rhi_texture_usage_to_vk( desc->usage );
    img_ci.sharingMode            = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout          = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = vkCreateImage( vk.device, &img_ci, vk.alloc_cb, &slot->image );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "texture_create: vkCreateImage: %s", string_VkResult( r ) );
        return ( rhi_texture_t ){ RHI_NULL_HANDLE };
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements( vk.device, slot->image, &reqs );

    vk_mem_alloc_t alloc = { 0 };
    if ( !vk_mem_alloc( reqs, RHI_MEMORY_GPU_ONLY, 0, &alloc ) )
    {
        vkDestroyImage( vk.device, slot->image, vk.alloc_cb );
        slot->image = VK_NULL_HANDLE;
        return ( rhi_texture_t ){ RHI_NULL_HANDLE };
    }
    slot->memory = alloc.memory;

    r = vkBindImageMemory( vk.device, slot->image, slot->memory, alloc.offset );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "texture_create: vkBindImageMemory: %s", string_VkResult( r ) );
        vkFreeMemory( vk.device, slot->memory, vk.alloc_cb );
        vkDestroyImage( vk.device, slot->image, vk.alloc_cb );
        slot->memory = VK_NULL_HANDLE;
        slot->image  = VK_NULL_HANDLE;
        return ( rhi_texture_t ){ RHI_NULL_HANDLE };
    }

    /* Default full-range view covering all mips and layers. */
    VkImageAspectFlags aspect = rhi_format_is_depth( desc->format )
                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                              : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageViewType view_type;
    if ( depth > 1 )                view_type = VK_IMAGE_VIEW_TYPE_3D;
    else if ( array_layers > 1 )    view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else                            view_type = VK_IMAGE_VIEW_TYPE_2D;

    VkImageViewCreateInfo view_ci           = { 0 };
    view_ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image                           = slot->image;
    view_ci.viewType                        = view_type;
    view_ci.format                          = vk_fmt;
    view_ci.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.subresourceRange.aspectMask     = aspect;
    view_ci.subresourceRange.baseMipLevel   = 0;
    view_ci.subresourceRange.levelCount     = mips;
    view_ci.subresourceRange.baseArrayLayer = 0;
    view_ci.subresourceRange.layerCount     = array_layers;

    r = vkCreateImageView( vk.device, &view_ci, vk.alloc_cb, &slot->view );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "texture_create: vkCreateImageView: %s", string_VkResult( r ) );
        vkFreeMemory( vk.device, slot->memory, vk.alloc_cb );
        vkDestroyImage( vk.device, slot->image, vk.alloc_cb );
        slot->memory = VK_NULL_HANDLE;
        slot->image  = VK_NULL_HANDLE;
        return ( rhi_texture_t ){ RHI_NULL_HANDLE };
    }

    if ( desc->debug_name )
        vk_debug_name_object( VK_OBJECT_TYPE_IMAGE, (u64)slot->image, desc->debug_name );

    slot->vk_format = vk_fmt;
    slot->width     = desc->width;
    slot->height    = desc->height;

    return ( rhi_texture_t ){ (u32)idx };
}

static void
vk_texture_destroy( rhi_texture_t handle )
{
    if ( !vk_texture_validate( handle ) )
        return;

    vk_texture_slot_t* slot = &vk.textures[ handle.id ];

    if ( slot->view   != VK_NULL_HANDLE )
        vkDestroyImageView( vk.device, slot->view,   vk.alloc_cb );
    if ( slot->image  != VK_NULL_HANDLE )
        vkDestroyImage    ( vk.device, slot->image,  vk.alloc_cb );
    if ( slot->memory != VK_NULL_HANDLE )
        vkFreeMemory      ( vk.device, slot->memory, vk.alloc_cb );

    slot->image  = VK_NULL_HANDLE;
    slot->view   = VK_NULL_HANDLE;
    slot->memory = VK_NULL_HANDLE;
}

/*==============================================================================================
    Sampler creation / destruction
==============================================================================================*/

static i32
vk_sampler_alloc_slot( void )
{
    for ( u32 i = 1; i < VK_MAX_SAMPLERS; ++i )
    {
        if ( vk.samplers[ i ].sampler == VK_NULL_HANDLE )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_sampler_validate( rhi_sampler_t handle )
{
    return handle.id > 0 && handle.id < VK_MAX_SAMPLERS
        && vk.samplers[ handle.id ].sampler != VK_NULL_HANDLE;
}

static rhi_sampler_t
vk_sampler_create( const rhi_sampler_desc_t* desc )
{
    if ( !desc )
        return ( rhi_sampler_t ){ RHI_NULL_HANDLE };

    i32 idx = vk_sampler_alloc_slot();
    if ( idx < 0 )
    {
        LOG_ERROR( "sampler pool exhausted (VK_MAX_SAMPLERS = %d)", VK_MAX_SAMPLERS );
        return ( rhi_sampler_t ){ RHI_NULL_HANDLE };
    }

    vk_sampler_slot_t* slot = &vk.samplers[ (u32)idx ];

    VkSamplerCreateInfo ci     = { 0 };
    ci.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter               = rhi_filter_to_vk( desc->mag_filter );
    ci.minFilter               = rhi_filter_to_vk( desc->min_filter );
    ci.mipmapMode              = rhi_mip_filter_to_vk( desc->mip_filter );
    ci.addressModeU            = rhi_address_to_vk( desc->address_u );
    ci.addressModeV            = rhi_address_to_vk( desc->address_v );
    ci.addressModeW            = rhi_address_to_vk( desc->address_w );
    ci.anisotropyEnable        = desc->max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy           = desc->max_anisotropy;
    ci.minLod                  = desc->min_lod;
    ci.maxLod                  = desc->max_lod > 0.0f ? desc->max_lod : VK_LOD_CLAMP_NONE;
    ci.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.unnormalizedCoordinates = VK_FALSE;

    VkResult r = vkCreateSampler( vk.device, &ci, vk.alloc_cb, &slot->sampler );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "sampler_create: vkCreateSampler: %s", string_VkResult( r ) );
        return ( rhi_sampler_t ){ RHI_NULL_HANDLE };
    }

    return ( rhi_sampler_t ){ (u32)idx };
}

static void
vk_sampler_destroy( rhi_sampler_t handle )
{
    if ( !vk_sampler_validate( handle ) )
        return;

    vk_sampler_slot_t* slot = &vk.samplers[ handle.id ];

    vkDestroySampler( vk.device, slot->sampler, vk.alloc_cb );

    slot->sampler = VK_NULL_HANDLE;
}

/*============================================================================================*/
