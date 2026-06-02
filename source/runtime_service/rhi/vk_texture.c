/*==============================================================================================

    vulkan/vk_texture.c -- VkImage + VkImageView lifecycle and format mapping.

    Slot pool: g_vk.textures[ VK_MAX_TEXTURES ] (vk_texture_slot_t).
    Each texture gets a default "full-range" image view covering all mips and layers.
    Sampler objects are separate (see vk_sampler_create below).

    Mip level auto-compute: if desc->mip_levels == 0, floor(log2(max(w,h))) + 1.

==============================================================================================*/

/*==============================================================================================
    Format mapping  (rhi_format_t -> VkFormat)
==============================================================================================*/

static VkFormat
rhi_format_to_vk( rhi_format_t fmt )
{
    /* TODO: complete the mapping as formats are needed. */
    switch ( fmt )
    {
        case RHI_FORMAT_RGBA8_UNORM:         return VK_FORMAT_R8G8B8A8_UNORM;
        case RHI_FORMAT_RGBA8_SRGB:          return VK_FORMAT_R8G8B8A8_SRGB;
        case RHI_FORMAT_BGRA8_UNORM:         return VK_FORMAT_B8G8R8A8_UNORM;
        case RHI_FORMAT_BGRA8_SRGB:          return VK_FORMAT_B8G8R8A8_SRGB;
        case RHI_FORMAT_RGBA16_FLOAT:        return VK_FORMAT_R16G16B16A16_SFLOAT;
        case RHI_FORMAT_RG11B10_FLOAT:       return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case RHI_FORMAT_RGB9E5_FLOAT:        return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case RHI_FORMAT_R8_UNORM:            return VK_FORMAT_R8_UNORM;
        case RHI_FORMAT_R16_FLOAT:           return VK_FORMAT_R16_SFLOAT;
        case RHI_FORMAT_R32_FLOAT:           return VK_FORMAT_R32_SFLOAT;
        case RHI_FORMAT_RG8_UNORM:           return VK_FORMAT_R8G8_UNORM;
        case RHI_FORMAT_RG16_FLOAT:          return VK_FORMAT_R16G16_SFLOAT;
        case RHI_FORMAT_RG32_FLOAT:          return VK_FORMAT_R32G32_SFLOAT;
        case RHI_FORMAT_D32_FLOAT:           return VK_FORMAT_D32_SFLOAT;
        case RHI_FORMAT_D24_UNORM_S8_UINT:   return VK_FORMAT_D24_UNORM_S8_UINT;
        case RHI_FORMAT_D16_UNORM:           return VK_FORMAT_D16_UNORM;
        default:                             return VK_FORMAT_UNDEFINED;
    }
}

static bool
rhi_format_is_depth( rhi_format_t fmt )
{
    return fmt == RHI_FORMAT_D32_FLOAT
        || fmt == RHI_FORMAT_D24_UNORM_S8_UINT
        || fmt == RHI_FORMAT_D16_UNORM;
}

/*==============================================================================================
    Slot allocation helpers
==============================================================================================*/

static i32
vk_texture_alloc_slot( void )
{
    for ( u32 i = 0; i < VK_MAX_TEXTURES; ++i )
    {
        if ( g_vk.textures[ i ].generation == 0 )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_texture_validate( rhi_texture_t handle )
{
    u32 idx = VK_HANDLE_IDX( handle.id );
    u8  gen = VK_HANDLE_GEN( handle.id );
    return idx < VK_MAX_TEXTURES && gen != 0 && g_vk.textures[ idx ].generation == gen;
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

    vk_texture_slot_t* slot = &g_vk.textures[ idx ];
    u8 gen = ( u8 )( slot->generation == 0 ? 1 : slot->generation );

    u16 mips = desc->mip_levels;
    if ( mips == 0 )
    {
        /* floor(log2(max(w,h))) + 1 */
        u32 dim = MAX( desc->width, desc->height );
        mips    = 1;
        while ( dim > 1 ) { dim >>= 1; mips++; }
    }

    VkFormat vk_fmt = rhi_format_to_vk( desc->format );

    /* TODO (Vulkan implementation):
       VkImageType img_type = (desc->depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
       VkImageUsageFlags vk_usage = rhi_texture_usage_to_vk( desc->usage );

       VkImageCreateInfo ci = {
           .imageType     = img_type,
           .format        = vk_fmt,
           .extent        = { desc->width, desc->height, MAX(desc->depth, 1) },
           .mipLevels     = mips,
           .arrayLayers   = MAX(desc->array_layers, 1),
           .samples       = VK_SAMPLE_COUNT_1_BIT,
           .tiling        = VK_IMAGE_TILING_OPTIMAL,
           .usage         = vk_usage,
           .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       };
       vkCreateImage( g_vk.device, &ci, g_vk.alloc_cb, &slot->image )

       VkMemoryRequirements2 reqs2;
       vkGetImageMemoryRequirements2( ... )
       vk_mem_alloc( reqs2.memoryRequirements, RHI_MEMORY_GPU_ONLY, &alloc )
       vkBindImageMemory( g_vk.device, slot->image, alloc.memory, alloc.offset )

       VkImageAspectFlags aspect = rhi_format_is_depth( desc->format )
                                 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                 : VK_IMAGE_ASPECT_COLOR_BIT;
       VkImageViewCreateInfo vci = {
           .image            = slot->image,
           .viewType         = (desc->array_layers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                        : VK_IMAGE_VIEW_TYPE_2D,
           .format           = vk_fmt,
           .subresourceRange = { aspect, 0, mips, 0, MAX(desc->array_layers,1) },
       };
       vkCreateImageView( g_vk.device, &vci, g_vk.alloc_cb, &slot->view )

       if ( desc->debug_name )
           vk_debug_name_object( VK_OBJECT_TYPE_IMAGE, (u64)slot->image, desc->debug_name )
    */

    slot->vk_format  = vk_fmt;
    slot->width      = desc->width;
    slot->height     = desc->height;
    slot->generation = gen;

    return ( rhi_texture_t ){ VK_MAKE_HANDLE( gen, ( u32 )idx ) };
}

static void
vk_texture_destroy( rhi_texture_t handle )
{
    if ( !vk_texture_validate( handle ) )
        return;

    u32                idx  = VK_HANDLE_IDX( handle.id );
    vk_texture_slot_t* slot = &g_vk.textures[ idx ];

    /* TODO:
       vkDestroyImageView( g_vk.device, slot->view,   g_vk.alloc_cb )
       vkDestroyImage    ( g_vk.device, slot->image,  g_vk.alloc_cb )
       vk_mem_free( ... )
    */

    slot->generation = ( u8 )( slot->generation + 1 );
    slot->image      = VK_NULL_HANDLE;
    slot->view       = VK_NULL_HANDLE;
}

/*==============================================================================================
    Sampler creation / destruction  (lives here for format/filter locality)
==============================================================================================*/

static i32
vk_sampler_alloc_slot( void )
{
    for ( u32 i = 0; i < VK_MAX_SAMPLERS; ++i )
    {
        if ( g_vk.samplers[ i ].generation == 0 )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_sampler_validate( rhi_sampler_t handle )
{
    u32 idx = VK_HANDLE_IDX( handle.id );
    u8  gen = VK_HANDLE_GEN( handle.id );
    return idx < VK_MAX_SAMPLERS && gen != 0 && g_vk.samplers[ idx ].generation == gen;
}

static VkFilter
rhi_filter_to_vk( rhi_filter_t f )
{
    return f == RHI_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static VkSamplerMipmapMode
rhi_mip_filter_to_vk( rhi_filter_t f )
{
    return f == RHI_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

static VkSamplerAddressMode
rhi_address_to_vk( rhi_address_mode_t m )
{
    switch ( m )
    {
        case RHI_ADDRESS_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case RHI_ADDRESS_MODE_CLAMP_TO_EDGE:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case RHI_ADDRESS_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:                               return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
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

    vk_sampler_slot_t* slot = &g_vk.samplers[ idx ];
    u8 gen = ( u8 )( slot->generation == 0 ? 1 : slot->generation );

    /* TODO:
       VkSamplerCreateInfo ci = {
           .magFilter               = rhi_filter_to_vk( desc->mag_filter ),
           .minFilter               = rhi_filter_to_vk( desc->min_filter ),
           .mipmapMode              = rhi_mip_filter_to_vk( desc->mip_filter ),
           .addressModeU            = rhi_address_to_vk( desc->address_u ),
           .addressModeV            = rhi_address_to_vk( desc->address_v ),
           .addressModeW            = rhi_address_to_vk( desc->address_w ),
           .anisotropyEnable        = desc->max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE,
           .maxAnisotropy           = desc->max_anisotropy,
           .minLod                  = desc->min_lod,
           .maxLod                  = desc->max_lod > 0.0f ? desc->max_lod : VK_LOD_CLAMP_NONE,
           .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
           .unnormalizedCoordinates = VK_FALSE,
       };
       vkCreateSampler( g_vk.device, &ci, g_vk.alloc_cb, &slot->sampler )
    */

    UNUSED( rhi_filter_to_vk );
    UNUSED( rhi_mip_filter_to_vk );
    UNUSED( rhi_address_to_vk );

    slot->generation = gen;
    return ( rhi_sampler_t ){ VK_MAKE_HANDLE( gen, ( u32 )idx ) };
}

static void
vk_sampler_destroy( rhi_sampler_t handle )
{
    if ( !vk_sampler_validate( handle ) )
        return;

    u32                idx  = VK_HANDLE_IDX( handle.id );
    vk_sampler_slot_t* slot = &g_vk.samplers[ idx ];

    /* TODO: vkDestroySampler( g_vk.device, slot->sampler, g_vk.alloc_cb ) */

    slot->generation = ( u8 )( slot->generation + 1 );
    slot->sampler    = VK_NULL_HANDLE;
}

/*============================================================================================*/
