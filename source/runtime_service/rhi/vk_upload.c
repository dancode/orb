/*==============================================================================================

    vulkan/vk_upload.c -- Staged upload ring buffer for GPU-only resources.

    One staging buffer per frame-in-flight slot (VK_STAGING_SIZE bytes each).
    The slot indexed by (vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT) is the active one.

    Upload flow:
        1. Caller calls rhi()->upload_buffer / upload_texture.
        2. Data is memcpy'd into the staging ring at the current head.
        3. Copy commands are recorded into the slot's dedicated transfer command buffer.
        4. vk_upload_flush (called at frame_begin after the fence wait) submits the
           pending transfer commands on the graphics queue and recycles the slot.

    Known issue: vk_upload_flush calls vkQueueWaitIdle after submitting the upload batch.
    This stalls the entire GPU queue until all uploads complete before render work can start.
    The frame fence from vkWaitForFences does NOT cover this; it only tracks the render
    command buffer from a past frame.  The upload command buffer is submitted here without
    any fence, so vkQueueWaitIdle is the only thing ensuring uploads finish before render
    commands read the results.

    Planned fix: attach a VkSemaphore (or timeline semaphore) to the upload vkQueueSubmit2
    and pass it as a pWaitSemaphoreInfos entry on the render submit.  The GPU will stall
    only the pipeline stages that consume uploaded data, letting other GPU work overlap.
    This matters most for a streaming engine that uploads assets every frame.

==============================================================================================*/

/* Per-slot state for the transfer command pool and recording status.
   Lives here rather than in vk_staging_t to keep vk_state.c self-contained. */
typedef struct vk_upload_slot_s
{
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    bool            is_recording;

} vk_upload_slot_t;

static vk_upload_slot_t g_upload[ VK_MAX_FRAMES_IN_FLIGHT ];

/*==============================================================================================
    Init / shutdown
==============================================================================================*/

static void vk_upload_shutdown( void );

static bool
vk_upload_init( void )
{
    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        /* Staging buffer: host-visible, coherent, transfer-source only. */
        VkBufferCreateInfo buf_ci = { 0 };
        buf_ci.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size               = VK_STAGING_SIZE;
        buf_ci.usage              = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buf_ci.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;

        VkResult r = vkCreateBuffer( vk.device, &buf_ci, vk.alloc_cb, &vk.staging[ i ].buffer );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkCreateBuffer[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }

        VkMemoryRequirements reqs;
        vkGetBufferMemoryRequirements( vk.device, vk.staging[ i ].buffer, &reqs );

        vk_mem_alloc_t alloc = { 0 };
        if ( !vk_mem_alloc( reqs, RHI_MEMORY_CPU_ONLY, &alloc ) )
            goto fail;
        vk.staging[ i ].memory = alloc.memory;

        r = vkBindBufferMemory( vk.device, vk.staging[ i ].buffer, vk.staging[ i ].memory, alloc.offset );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkBindBufferMemory[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }

        r = vkMapMemory( vk.device, vk.staging[ i ].memory, 0, VK_STAGING_SIZE, 0, &vk.staging[ i ].mapped );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkMapMemory[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }
        vk.staging[ i ].head = 0;

        /* Dedicated command pool for transfer recording (graphics family for queue compatibility). */
        VkCommandPoolCreateInfo pool_ci = { 0 };
        pool_ci.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_ci.queueFamilyIndex        = vk.graphics_queue_family;

        r = vkCreateCommandPool( vk.device, &pool_ci, vk.alloc_cb, &g_upload[ i ].pool );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkCreateCommandPool[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }

        VkCommandBufferAllocateInfo alloc_info = { 0 };
        alloc_info.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool                 = g_upload[ i ].pool;
        alloc_info.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount          = 1;

        r = vkAllocateCommandBuffers( vk.device, &alloc_info, &g_upload[ i ].cmd );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkAllocateCommandBuffers[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }

        g_upload[ i ].is_recording = false;
    }

    LOG_INFO( "upload_init: OK (%u slots, %u MB each)",
              VK_MAX_FRAMES_IN_FLIGHT, (u32)( VK_STAGING_SIZE / ( 1024 * 1024 ) ) );
    return true;

fail:
    vk_upload_shutdown();
    return false;
}

static void
vk_upload_shutdown( void )
{
    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        if ( g_upload[ i ].pool != VK_NULL_HANDLE )
        {
            vkDestroyCommandPool( vk.device, g_upload[ i ].pool, vk.alloc_cb );
            g_upload[ i ].pool         = VK_NULL_HANDLE;
            g_upload[ i ].cmd          = VK_NULL_HANDLE;
            g_upload[ i ].is_recording = false;
        }
        if ( vk.staging[ i ].mapped )
        {
            vkUnmapMemory( vk.device, vk.staging[ i ].memory );
            vk.staging[ i ].mapped = NULL;
        }
        if ( vk.staging[ i ].buffer != VK_NULL_HANDLE )
        {
            vkDestroyBuffer( vk.device, vk.staging[ i ].buffer, vk.alloc_cb );
            vk.staging[ i ].buffer = VK_NULL_HANDLE;
        }
        if ( vk.staging[ i ].memory != VK_NULL_HANDLE )
        {
            vkFreeMemory( vk.device, vk.staging[ i ].memory, vk.alloc_cb );
            vk.staging[ i ].memory = VK_NULL_HANDLE;
        }
        vk.staging[ i ].head = 0;
    }
}

/*==============================================================================================
    Alloc from the active staging slot
==============================================================================================*/

typedef struct vk_staging_alloc_s
{
    void*         cpu_ptr;
    VkBuffer      buffer;
    VkDeviceSize  offset;

} vk_staging_alloc_t;

static void
vk_upload_begin_recording( u32 slot )
{
    vk_upload_slot_t* up = &g_upload[ slot ];
    if ( up->is_recording )
        return;

    VkCommandBufferBeginInfo bi = { 0 };
    bi.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer( up->cmd, &bi );
    up->is_recording = true;
}

static bool
vk_staging_alloc( u32 size, u32 align, vk_staging_alloc_t* out )
{
    u32            slot = vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT;
    vk_staging_t*  s    = &vk.staging[ slot ];

    u32 aligned_head = ( s->head + align - 1 ) & ~( align - 1 );
    if ( aligned_head + size > VK_STAGING_SIZE )
    {
        LOG_ERROR( "vk_staging_alloc: ring full (slot=%u used=%u req=%u cap=%u)",
                   slot, aligned_head, size, (u32)VK_STAGING_SIZE );
        return false;
    }

    out->cpu_ptr  = (u8*)s->mapped + aligned_head;
    out->buffer   = s->buffer;
    out->offset   = aligned_head;
    s->head       = aligned_head + size;

    vk_upload_begin_recording( slot );
    return true;
}

/*==============================================================================================
    Upload helpers  (called via rhi_api.c wiring)
==============================================================================================*/

static bool
vk_upload_buffer( rhi_buffer_t dst, const void* data, u32 size )
{
    if ( !vk_buffer_validate( dst ) || !data || size == 0 )
        return false;

    vk_staging_alloc_t sa;
    if ( !vk_staging_alloc( size, 4, &sa ) )
        return false;

    memcpy( sa.cpu_ptr, data, size );

    u32             slot    = vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT;
    VkCommandBuffer cmd     = g_upload[ slot ].cmd;
    VkBuffer        dst_buf = vk.buffers[ dst.id ].buffer;

    VkBufferCopy region = { 0 };
    region.srcOffset    = sa.offset;
    region.dstOffset    = 0;
    region.size         = size;
    vkCmdCopyBuffer( cmd, sa.buffer, dst_buf, 1, &region );

    return true;
}

static bool
vk_upload_texture( rhi_texture_t dst, const void* data, u32 data_size, u16 mip, u16 layer )
{
    if ( !vk_texture_validate( dst ) || !data || data_size == 0 )
        return false;

    vk_staging_alloc_t sa;
    if ( !vk_staging_alloc( data_size, 16, &sa ) )
        return false;

    memcpy( sa.cpu_ptr, data, data_size );

    u32                slot = vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT;
    VkCommandBuffer    cmd  = g_upload[ slot ].cmd;
    vk_texture_slot_t* tex  = &vk.textures[ dst.id ];

    u32 mip_w = tex->width  >> mip; if ( mip_w < 1 ) mip_w = 1;
    u32 mip_h = tex->height >> mip; if ( mip_h < 1 ) mip_h = 1;

    /* Barrier: UNDEFINED -> TRANSFER_DST_OPTIMAL */
    VkImageMemoryBarrier2 to_dst               = { 0 };
    to_dst.sType                               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_dst.srcStageMask                        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_dst.srcAccessMask                       = 0;
    to_dst.dstStageMask                        = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_dst.dstAccessMask                       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_dst.oldLayout                           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout                           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image                               = tex->image;
    to_dst.subresourceRange.aspectMask         = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.baseMipLevel       = mip;
    to_dst.subresourceRange.levelCount         = 1;
    to_dst.subresourceRange.baseArrayLayer     = layer;
    to_dst.subresourceRange.layerCount         = 1;

    VkDependencyInfo dep_to_dst        = { 0 };
    dep_to_dst.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_to_dst.imageMemoryBarrierCount = 1;
    dep_to_dst.pImageMemoryBarriers    = &to_dst;
    vkCmdPipelineBarrier2( cmd, &dep_to_dst );

    /* Copy buffer -> image. */
    VkBufferImageCopy region                    = { 0 };
    region.bufferOffset                         = sa.offset;
    region.imageSubresource.aspectMask          = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel            = mip;
    region.imageSubresource.baseArrayLayer      = layer;
    region.imageSubresource.layerCount          = 1;
    region.imageExtent.width                    = mip_w;
    region.imageExtent.height                   = mip_h;
    region.imageExtent.depth                    = 1;
    vkCmdCopyBufferToImage( cmd, sa.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

    /* Barrier: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL */
    VkImageMemoryBarrier2 to_read               = to_dst;
    to_read.srcStageMask                        = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_read.srcAccessMask                       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask                        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    to_read.dstAccessMask                       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout                           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout                           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDependencyInfo dep_to_read        = { 0 };
    dep_to_read.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_to_read.imageMemoryBarrierCount = 1;
    dep_to_read.pImageMemoryBarriers    = &to_read;
    vkCmdPipelineBarrier2( cmd, &dep_to_read );

    return true;
}

/*==============================================================================================
    Flush  (called from vk_frame.c at the top of frame_begin after fence wait)
==============================================================================================*/

static void
vk_upload_flush( u32 slot )
{
    if ( slot >= VK_MAX_FRAMES_IN_FLIGHT )
        return;

    vk_upload_slot_t* up = &g_upload[ slot ];

    if ( up->is_recording )
    {
        vkEndCommandBuffer( up->cmd );

        VkCommandBufferSubmitInfo cmd_si = { 0 };
        cmd_si.sType                     = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_si.commandBuffer             = up->cmd;

        VkSubmitInfo2 submit             = { 0 };
        submit.sType                     = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount    = 1;
        submit.pCommandBufferInfos       = &cmd_si;

        VkResult r = vkQueueSubmit2( vk.graphics_queue, 1, &submit, VK_NULL_HANDLE );
        if ( r != VK_SUCCESS )
            LOG_ERROR( "upload_flush: vkQueueSubmit2: %s", string_VkResult( r ) );
        else
            /* KNOWN ISSUE: stalls the entire GPU queue until uploads complete.
               Replace with a per-slot semaphore waited on by the render submit. */
            vkQueueWaitIdle( vk.graphics_queue );

        vkResetCommandBuffer( up->cmd, 0 );
        up->is_recording = false;
    }

    vk.staging[ slot ].head = 0;
}

/*============================================================================================*/
