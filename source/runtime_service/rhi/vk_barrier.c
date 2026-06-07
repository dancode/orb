/*==============================================================================================

    vk_barrier.c -- Image layout transition barriers (cmd_image_barrier).

    Translates rhi_layout_t pairs into VkImageMemoryBarrier2 records and submits them
    via vkCmdPipelineBarrier2 (synchronization2, promoted in Vulkan 1.3).

    Each rhi_layout_t has two scope halves:
        src  -- what the image was doing; stage/access that must finish before the barrier
        dst  -- what the image will do;   stage/access that must wait until after the barrier

    The barrier uses old_layout's src scope and new_layout's dst scope to form a precise
    execution + memory dependency.  Stages are the minimal covering set for each layout;
    no unnecessary pipeline stages are blocked.

    Internal barriers (swapchain, context depth) remain in vk_frame.c and are unaffected.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Layout scope

    VkPipelineStageFlags2 and VkAccessFlags2 are 64-bit typedef'd integers whose named
    constants are declared as 'static const' in the Vulkan headers.  MSVC C rejects
    'static const' variables as static initializers, so the scope table is implemented
    as a switch function rather than a const array.  All assignments happen in function
    scope where runtime values are legal.
==============================================================================================*/

typedef struct vk_layout_scope_s
{
    VkImageLayout           layout;
    VkPipelineStageFlags2   src_stage;    /* stage to wait on (old-layout side)  */
    VkAccessFlags2          src_access;   /* access to flush  (old-layout side)  */
    VkPipelineStageFlags2   dst_stage;    /* stage to unblock (new-layout side)  */
    VkAccessFlags2          dst_access;   /* access to expose (new-layout side)  */

} vk_layout_scope_t;

static vk_layout_scope_t
vk_layout_scope_get( rhi_layout_t layout )
{
    vk_layout_scope_t s = { 0 };
    switch ( layout )
    {
    /* UNDEFINED -- no prior work; contents are discarded on transition */
    default:
    case RHI_LAYOUT_UNDEFINED:
        s.layout     = VK_IMAGE_LAYOUT_UNDEFINED;
        s.src_stage  = VK_PIPELINE_STAGE_2_NONE;
        s.src_access = 0;
        s.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        s.dst_access = 0;
        break;

    /* COLOR_ATTACHMENT */
    case RHI_LAYOUT_COLOR_ATTACHMENT:
        s.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        s.src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        s.dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
                     | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;

    /* DEPTH_ATTACHMENT -- full read/write depth testing */
    case RHI_LAYOUT_DEPTH_ATTACHMENT:
        s.layout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        s.src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        s.dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;

    /* DEPTH_READ_ONLY -- depth test active; no writes; may also be sampled */
    case RHI_LAYOUT_DEPTH_READ_ONLY:
        s.layout     = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        s.src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        s.dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                     | VK_ACCESS_2_SHADER_READ_BIT;
        break;

    /* SHADER_READ -- sampled in any shader stage */
    case RHI_LAYOUT_SHADER_READ:
        s.layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                     | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        s.src_access = VK_ACCESS_2_SHADER_READ_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                     | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        s.dst_access = VK_ACCESS_2_SHADER_READ_BIT;
        break;

    /* STORAGE -- read/write via image load/store in compute */
    case RHI_LAYOUT_STORAGE:
        s.layout     = VK_IMAGE_LAYOUT_GENERAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        s.src_access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        s.dst_access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        break;

    /* TRANSFER_SRC -- source of a copy or blit */
    case RHI_LAYOUT_TRANSFER_SRC:
        s.layout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        s.src_access = VK_ACCESS_2_TRANSFER_READ_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        s.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;

    /* TRANSFER_DST -- destination of a copy; layout used internally by upload_texture */
    case RHI_LAYOUT_TRANSFER_DST:
        s.layout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        s.src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        s.src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        s.dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        s.dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    }
    return s;
}

/*==============================================================================================
    Aspect detection  (derived from the actual VkFormat stored in the texture slot)
==============================================================================================*/

static VkImageAspectFlags
vk_format_aspect( VkFormat fmt )
{
    switch ( fmt )
    {
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM:         return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT
                                               | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:                          return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/*==============================================================================================
    cmd_image_barrier
==============================================================================================*/

#define VK_MAX_BARRIER_BATCH   16

static void
vk_cmd_image_barrier( rhi_cmd_t cmd, const rhi_image_barrier_t* barriers, u32 count )
{
    if ( !cmd || !barriers || count == 0 )
        return;

    if ( count > VK_MAX_BARRIER_BATCH )
    {
        LOG_WARN( "cmd_image_barrier: count %u exceeds batch limit (%u); clamping",
                  count, VK_MAX_BARRIER_BATCH );
        count = VK_MAX_BARRIER_BATCH;
    }

    VkImageMemoryBarrier2 vk_barriers[ VK_MAX_BARRIER_BATCH ] = { 0 };
    u32 vk_count = 0;

    for ( u32 i = 0; i < count; ++i )
    {
        const rhi_image_barrier_t* b = &barriers[ i ];

        /* Validate texture handle and that the slot is populated. */
        if ( b->texture.id <= 0 || b->texture.id >= VK_MAX_TEXTURES
             || vk.textures[ b->texture.id ].image == VK_NULL_HANDLE )
        {
            LOG_WARN( "cmd_image_barrier: barriers[%u]: invalid texture handle %d; skipping",
                      i, b->texture.id );
            continue;
        }

        /* Same-layout transition is a no-op. */
        if ( b->old_layout == b->new_layout )
            continue;

        vk_layout_scope_t src        = vk_layout_scope_get( b->old_layout );
        vk_layout_scope_t dst        = vk_layout_scope_get( b->new_layout );
        const vk_texture_slot_t* tex = &vk.textures[ b->texture.id ];

        VkImageMemoryBarrier2* vb               = &vk_barriers[ vk_count++ ];
        vb->sType                               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        vb->srcStageMask                        = src.src_stage;
        vb->srcAccessMask                       = src.src_access;
        vb->dstStageMask                        = dst.dst_stage;
        vb->dstAccessMask                       = dst.dst_access;
        vb->oldLayout                           = src.layout;
        vb->newLayout                           = dst.layout;
        vb->srcQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
        vb->dstQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
        vb->image                               = tex->image;
        vb->subresourceRange.aspectMask         = vk_format_aspect( tex->vk_format );
        vb->subresourceRange.baseMipLevel       = 0;
        vb->subresourceRange.levelCount         = VK_REMAINING_MIP_LEVELS;
        vb->subresourceRange.baseArrayLayer     = 0;
        vb->subresourceRange.layerCount         = VK_REMAINING_ARRAY_LAYERS;
    }

    if ( vk_count == 0 )
        return;

    VkDependencyInfo dep        = { 0 };
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = vk_count;
    dep.pImageMemoryBarriers    = vk_barriers;

    vkCmdPipelineBarrier2( cmd->vk_cmd, &dep );
}

/*============================================================================================*/
// clang-format on
