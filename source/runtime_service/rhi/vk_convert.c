/*==============================================================================================

    vk_convert.c -- Stateless RHI -> Vulkan enum and format translation helpers.

    All functions here are pure conversions: they take an RHI enum or flags value and
    return the Vulkan equivalent.  No state is read or written.

    Grouped by domain:
        Pixel formats       rhi_format_to_vk, rhi_format_is_depth
        Texture usage       rhi_texture_usage_to_vk
        Sampler             rhi_filter_to_vk, rhi_mip_filter_to_vk, rhi_address_to_vk
        Buffer usage        rhi_buffer_usage_to_vk
        Pipeline state      rhi_cull_to_vk, rhi_compare_to_vk, rhi_blend_factor_to_vk,
                            rhi_blend_op_to_vk, rhi_vertex_format_to_vk
        Render pass ops     vk_load_op, vk_store_op

==============================================================================================*/

/*==============================================================================================
    Pixel formats
==============================================================================================*/

static VkFormat
rhi_format_to_vk( rhi_format_t fmt )
{
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
    Texture usage
==============================================================================================*/

static VkImageUsageFlags
rhi_texture_usage_to_vk( rhi_texture_usage_t usage )
{
    VkImageUsageFlags flags = 0;
    if ( usage & RHI_TEXTURE_USAGE_SAMPLED          ) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ( usage & RHI_TEXTURE_USAGE_STORAGE          ) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ( usage & RHI_TEXTURE_USAGE_COLOR_ATTACHMENT ) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ( usage & RHI_TEXTURE_USAGE_DEPTH_ATTACHMENT ) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ( usage & RHI_TEXTURE_USAGE_TRANSFER_SRC     ) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ( usage & RHI_TEXTURE_USAGE_TRANSFER_DST     ) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

/*==============================================================================================
    Sampler
==============================================================================================*/

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

/*==============================================================================================
    Buffer usage
==============================================================================================*/

static VkBufferUsageFlags
rhi_buffer_usage_to_vk( rhi_buffer_usage_t usage )
{
    VkBufferUsageFlags flags = 0;
    if ( usage & RHI_BUFFER_USAGE_VERTEX         ) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if ( usage & RHI_BUFFER_USAGE_INDEX          ) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if ( usage & RHI_BUFFER_USAGE_UNIFORM        ) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if ( usage & RHI_BUFFER_USAGE_STORAGE        ) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if ( usage & RHI_BUFFER_USAGE_INDIRECT       ) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if ( usage & RHI_BUFFER_USAGE_TRANSFER_SRC   ) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if ( usage & RHI_BUFFER_USAGE_TRANSFER_DST   ) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ( usage & RHI_BUFFER_USAGE_DEVICE_ADDRESS ) flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return flags;
}

/*==============================================================================================
    Pipeline state
==============================================================================================*/

static VkCullModeFlags
rhi_cull_to_vk( rhi_cull_mode_t cull )
{
    switch ( cull )
    {
        case RHI_CULL_FRONT: return VK_CULL_MODE_FRONT_BIT;
        case RHI_CULL_BACK:  return VK_CULL_MODE_BACK_BIT;
        default:             return VK_CULL_MODE_NONE;
    }
}

static VkCompareOp
rhi_compare_to_vk( rhi_compare_op_t op )
{
    switch ( op )
    {
        case RHI_COMPARE_NEVER:         return VK_COMPARE_OP_NEVER;
        case RHI_COMPARE_LESS:          return VK_COMPARE_OP_LESS;
        case RHI_COMPARE_EQUAL:         return VK_COMPARE_OP_EQUAL;
        case RHI_COMPARE_LESS_EQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case RHI_COMPARE_GREATER:       return VK_COMPARE_OP_GREATER;
        case RHI_COMPARE_NOT_EQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
        case RHI_COMPARE_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:                        return VK_COMPARE_OP_ALWAYS;
    }
}

static VkBlendFactor
rhi_blend_factor_to_vk( rhi_blend_factor_t f )
{
    switch ( f )
    {
        case RHI_BLEND_ZERO:            return VK_BLEND_FACTOR_ZERO;
        case RHI_BLEND_ONE:             return VK_BLEND_FACTOR_ONE;
        case RHI_BLEND_SRC_COLOR:       return VK_BLEND_FACTOR_SRC_COLOR;
        case RHI_BLEND_ONE_MINUS_SRC_C: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case RHI_BLEND_SRC_ALPHA:       return VK_BLEND_FACTOR_SRC_ALPHA;
        case RHI_BLEND_ONE_MINUS_SRC_A: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case RHI_BLEND_DST_ALPHA:       return VK_BLEND_FACTOR_DST_ALPHA;
        case RHI_BLEND_ONE_MINUS_DST_A: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:                        return VK_BLEND_FACTOR_ZERO;
    }
}

static VkBlendOp
rhi_blend_op_to_vk( rhi_blend_op_t op )
{
    switch ( op )
    {
        case RHI_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case RHI_BLEND_OP_MIN:      return VK_BLEND_OP_MIN;
        case RHI_BLEND_OP_MAX:      return VK_BLEND_OP_MAX;
        default:                    return VK_BLEND_OP_ADD;
    }
}

static VkFormat
rhi_vertex_format_to_vk( rhi_vertex_format_t f )
{
    switch ( f )
    {
        case RHI_VERTEX_FORMAT_FLOAT:  return VK_FORMAT_R32_SFLOAT;
        case RHI_VERTEX_FORMAT_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
        case RHI_VERTEX_FORMAT_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
        case RHI_VERTEX_FORMAT_FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case RHI_VERTEX_FORMAT_UINT:   return VK_FORMAT_R32_UINT;
        case RHI_VERTEX_FORMAT_UINT2:  return VK_FORMAT_R32G32_UINT;
        case RHI_VERTEX_FORMAT_UINT4:  return VK_FORMAT_R32G32B32A32_UINT;
        case RHI_VERTEX_FORMAT_UNORM4: return VK_FORMAT_R8G8B8A8_UNORM;
        default:                       return VK_FORMAT_UNDEFINED;
    }
}

/*==============================================================================================
    Render pass load / store ops
==============================================================================================*/

static VkAttachmentLoadOp
vk_load_op( rhi_load_op_t op )
{
    switch ( op )
    {
        case RHI_LOAD_OP_LOAD:    return VK_ATTACHMENT_LOAD_OP_LOAD;
        case RHI_LOAD_OP_CLEAR:   return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case RHI_LOAD_OP_DISCARD: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

static VkAttachmentStoreOp
vk_store_op( rhi_store_op_t op )
{
    switch ( op )
    {
        case RHI_STORE_OP_STORE:   return VK_ATTACHMENT_STORE_OP_STORE;
        case RHI_STORE_OP_DISCARD: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

/*============================================================================================*/
