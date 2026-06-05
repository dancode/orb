/*==============================================================================================

    vulkan/vk_upload.c -- Staged upload ring buffer for GPU-only resources.

    One staging buffer per frame-in-flight slot (VK_STAGING_SIZE bytes each).
    The active slot is tracked by g_upload_active_slot.  vk_upload_flush advances it, and
    vk_frame.c gates that call to fire at most once per display epoch via upload_flush_epoch,
    so the slot cycles at display-frame cadence regardless of how many contexts call frame_begin.

    Upload flow:
        1. Caller calls rhi()->upload_buffer / upload_texture.
        2. Data is memcpy'd into the staging ring at the current head.
        3. Copy commands are recorded into the slot's dedicated transfer command buffer.
        4. vk_upload_flush (called at frame_begin after the fence wait for this slot)
           submits the pending transfer commands to the transfer queue, resets the slot head,
           then advances g_upload_active_slot.  Because slots cycle every
           VK_MAX_FRAMES_IN_FLIGHT flushes, uploads queued in flush G are not overwritten
           until flush G+VK_MAX_FRAMES_IN_FLIGHT.

    Sync: vk_upload_flush signals vk.upload_timeline at ++vk.upload_counter when the DMA
    batch completes.  vk_frame_end adds a wait on that semaphore value at vertex-input,
    shader, and compute stages, so those GPU stages stall only until the DMA finishes --
    the 3D engine front-end (draw setup, early-Z) can overlap with the async transfer.
    Frames with no uploads skip the semaphore wait entirely.

    Queue-family ownership transfers (QFOTs):
        When vk.transfer_queue_family != vk.graphics_queue_family (dedicated DMA engine on
        discrete AMD/NVIDIA), resources uploaded via this path are on an EXCLUSIVE sharing
        mode.  After the copy, each resource needs a two-step QFOT:
            a. Release barrier -- recorded into the transfer command buffer after the copy.
               Relinquishes ownership; declares the intended final layout.
            b. Acquire barrier -- recorded at the top of the graphics command buffer in
               vk_upload_apply_acquires().  Completes the ownership transfer and performs
               the layout transition visible to shader stages.
        The timeline semaphore provides the cross-queue execution dependency; the acquire
        barrier provides the memory visibility dependency on the graphics queue.
        When the transfer family equals the graphics family (integrated GPU / fallback),
        no QFOT is needed: barriers use VK_QUEUE_FAMILY_IGNORED and all stages run on the
        same queue.

    Staging reuse safety: vk_staging_t.last_submit_value records the upload_timeline value
    signaled when a slot is submitted.  vk_staging_alloc checks this value via
    vkWaitSemaphores on the first write to a slot each cycle, guaranteeing the prior DMA read
    from that buffer has completed before the CPU memcpy overwrites it.

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
static u32              g_upload_active_slot = 0;

/* Pending queue-family ownership transfer acquires.
   Populated by vk_upload_texture/vk_upload_buffer when a dedicated transfer queue is in use.
   Drained by vk_upload_apply_acquires() at the top of each graphics command buffer.
   Global (not per-slot) so missed frames (early frame_begin returns) carry over correctly. */

#define VK_MAX_UPLOAD_ACQUIRES 256

typedef struct vk_image_acquire_s
{
    VkImage image;
    u16     mip;
    u16     layer;
} vk_image_acquire_t;

static vk_image_acquire_t g_pending_image_acquires[ VK_MAX_UPLOAD_ACQUIRES ];
static VkBuffer           g_pending_buffer_acquires[ VK_MAX_UPLOAD_ACQUIRES ];
static u32                g_pending_image_count;
static u32                g_pending_buffer_count;

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
        if ( !vk_mem_alloc( reqs, RHI_MEMORY_CPU_ONLY, 0, &alloc ) )
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

        /* Command pool on the transfer queue family.  Falls back to graphics when no dedicated
           transfer family exists; in that case transfer_queue_family == graphics_queue_family. */
        VkCommandPoolCreateInfo pool_ci = { 0 };
        pool_ci.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_ci.queueFamilyIndex        = vk.transfer_queue_family;

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

    /* Timeline semaphore: vk_upload_flush signals this after each DMA batch. */
    {
        VkSemaphoreTypeCreateInfo type_ci = { 0 };
        type_ci.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_ci.initialValue  = 0;

        VkSemaphoreCreateInfo sem_ci = { 0 };
        sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem_ci.pNext = &type_ci;

        VkResult r = vkCreateSemaphore( vk.device, &sem_ci, vk.alloc_cb, &vk.upload_timeline );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkCreateSemaphore (timeline): %s", string_VkResult( r ) );
            goto fail;
        }
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
    if ( vk.upload_timeline != VK_NULL_HANDLE )
    {
        vkDestroySemaphore( vk.device, vk.upload_timeline, vk.alloc_cb );
        vk.upload_timeline = VK_NULL_HANDLE;
    }
    vk.upload_counter = 0;

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
    VkResult r = vkBeginCommandBuffer( up->cmd, &bi );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "upload_begin_recording: vkBeginCommandBuffer: %s", string_VkResult( r ) );
        return;
    }
    up->is_recording = true;
}

static bool
vk_staging_alloc( u32 size, u32 align, vk_staging_alloc_t* out )
{
    u32            slot = g_upload_active_slot;
    vk_staging_t*  s    = &vk.staging[ slot ];

    /* First write to this slot in the new cycle: formally guarantee the prior DMA read from
       this buffer has completed.  The epoch-gated flush in vk_frame.c keeps the slot cycling
       at display-frame cadence, so this wait is normally already satisfied by the fence wait
       in frame_begin.  The explicit semaphore wait is kept as a belt-and-suspenders guarantee
       that remains correct regardless of future changes to context topology or flush ordering. */
    if ( s->head == 0 && s->last_submit_value > 0 )
    {
        VkSemaphoreWaitInfo wi = { 0 };
        wi.sType               = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount      = 1;
        wi.pSemaphores         = &vk.upload_timeline;
        wi.pValues             = &s->last_submit_value;
        vkWaitSemaphores( vk.device, &wi, UINT64_MAX );
    }

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

    u32             slot    = g_upload_active_slot;
    VkCommandBuffer cmd     = g_upload[ slot ].cmd;
    VkBuffer        dst_buf = vk.buffers[ dst.id ].buffer;

    VkBufferCopy region = { 0 };
    region.srcOffset    = sa.offset;
    region.dstOffset    = 0;
    region.size         = size;
    vkCmdCopyBuffer( cmd, sa.buffer, dst_buf, 1, &region );

    if ( vk.transfer_queue_family != vk.graphics_queue_family )
    {
        /* Release buffer ownership to the graphics family; matched by an acquire in
           vk_upload_apply_acquires on the graphics command buffer. */
        VkBufferMemoryBarrier2 release  = { 0 };
        release.sType                   = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        release.srcStageMask            = VK_PIPELINE_STAGE_2_COPY_BIT;
        release.srcAccessMask           = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        release.dstStageMask            = VK_PIPELINE_STAGE_2_NONE;
        release.dstAccessMask           = 0;
        release.srcQueueFamilyIndex     = vk.transfer_queue_family;
        release.dstQueueFamilyIndex     = vk.graphics_queue_family;
        release.buffer                  = dst_buf;
        release.offset                  = 0;
        release.size                    = VK_WHOLE_SIZE;

        VkDependencyInfo dep_rel         = { 0 };
        dep_rel.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_rel.bufferMemoryBarrierCount = 1;
        dep_rel.pBufferMemoryBarriers    = &release;
        vkCmdPipelineBarrier2( cmd, &dep_rel );

        if ( g_pending_buffer_count < VK_MAX_UPLOAD_ACQUIRES )
            g_pending_buffer_acquires[ g_pending_buffer_count++ ] = dst_buf;
        else
            LOG_WARN( "upload_buffer: pending acquire list full; buffer QFOT will be skipped" );
    }

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

    u32                slot = g_upload_active_slot;
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

    if ( vk.transfer_queue_family != vk.graphics_queue_family )
    {
        /* Dedicated transfer queue: release ownership to the graphics family.
           The layout transition (TRANSFER_DST -> SHADER_READ_ONLY) is declared in both
           the release and the matching acquire, which vk_upload_apply_acquires injects
           into the graphics command buffer. */
        VkImageMemoryBarrier2 release           = to_dst;
        release.srcStageMask                    = VK_PIPELINE_STAGE_2_COPY_BIT;
        release.srcAccessMask                   = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        release.dstStageMask                    = VK_PIPELINE_STAGE_2_NONE;
        release.dstAccessMask                   = 0;
        release.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        release.srcQueueFamilyIndex             = vk.transfer_queue_family;
        release.dstQueueFamilyIndex             = vk.graphics_queue_family;

        VkDependencyInfo dep_rel                = { 0 };
        dep_rel.sType                           = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_rel.imageMemoryBarrierCount         = 1;
        dep_rel.pImageMemoryBarriers            = &release;
        vkCmdPipelineBarrier2( cmd, &dep_rel );

        if ( g_pending_image_count < VK_MAX_UPLOAD_ACQUIRES )
            g_pending_image_acquires[ g_pending_image_count++ ] = (vk_image_acquire_t){ tex->image, mip, layer };
        else
            LOG_WARN( "upload_texture: pending acquire list full; image will not transition to SHADER_READ" );
    }
    else
    {
        /* Same queue family: no ownership transfer needed; transition the layout here. */
        VkImageMemoryBarrier2 to_read           = to_dst;
        to_read.srcStageMask                    = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask                   = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask                    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                                | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_read.dstAccessMask                   = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDependencyInfo dep_to_read            = { 0 };
        dep_to_read.sType                       = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_to_read.imageMemoryBarrierCount     = 1;
        dep_to_read.pImageMemoryBarriers        = &to_read;
        vkCmdPipelineBarrier2( cmd, &dep_to_read );
    }

    return true;
}

/*==============================================================================================
    Flush  (called from vk_frame.c at the top of frame_begin after fence wait)
==============================================================================================*/

static void
vk_upload_flush( void )
{
    u32                slot = g_upload_active_slot;
    vk_upload_slot_t*  up   = &g_upload[ slot ];

    if ( up->is_recording )
    {
        vkEndCommandBuffer( up->cmd );

        VkCommandBufferSubmitInfo cmd_si = { 0 };
        cmd_si.sType                     = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_si.commandBuffer             = up->cmd;

        /* Signal upload_timeline at the next counter value when the DMA batch completes. */
        u64 signal_value                 = vk.upload_counter + 1;

        VkSemaphoreSubmitInfo signal_si  = { 0 };
        signal_si.sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_si.semaphore              = vk.upload_timeline;
        signal_si.value                  = signal_value;
        signal_si.stageMask              = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;

        VkSubmitInfo2 submit             = { 0 };
        submit.sType                     = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount    = 1;
        submit.pCommandBufferInfos       = &cmd_si;
        submit.signalSemaphoreInfoCount  = 1;
        submit.pSignalSemaphoreInfos     = &signal_si;

        VkResult r = vkQueueSubmit2( vk.transfer_queue, 1, &submit, VK_NULL_HANDLE );
        if ( r != VK_SUCCESS )
            LOG_ERROR( "upload_flush: vkQueueSubmit2: %s", string_VkResult( r ) );
        else
        {
            vk.upload_counter                    = signal_value;
            vk.staging[ slot ].last_submit_value = signal_value;
        }

        vkResetCommandBuffer( up->cmd, 0 );
        up->is_recording = false;
    }

    vk.staging[ slot ].head  = 0;
    g_upload_active_slot     = ( slot + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
}

/*==============================================================================================
    Acquire injection  (called from vk_frame.c at the top of each graphics command buffer)
==============================================================================================*/

static void
vk_upload_apply_acquires( VkCommandBuffer cmd, i32 ctx_id )
{
    /* Same-family path: no QFOTs were issued, nothing to acquire. */
    if ( vk.transfer_queue_family == vk.graphics_queue_family )
        return;
    if ( g_pending_image_count == 0 && g_pending_buffer_count == 0 )
        return;

    /* Build image acquire barriers.  The matching release barriers declared
       TRANSFER_DST -> SHADER_READ_ONLY; the acquire must use the same layout pair.
       srcStageMask = NONE because the timeline semaphore wait in the graphics submit
       already provides the inter-queue execution dependency. */
    static VkImageMemoryBarrier2  img_bars[ VK_MAX_UPLOAD_ACQUIRES ];
    for ( u32 i = 0; i < g_pending_image_count; ++i )
    {
        vk_image_acquire_t*    e = &g_pending_image_acquires[ i ];
        VkImageMemoryBarrier2* b = &img_bars[ i ];

        *b = (VkImageMemoryBarrier2){ 0 };
        b->sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b->srcStageMask                    = VK_PIPELINE_STAGE_2_NONE;
        b->srcAccessMask                   = 0;
        b->dstStageMask                    = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                           | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                           | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b->dstAccessMask                   = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b->oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b->newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b->srcQueueFamilyIndex             = vk.transfer_queue_family;
        b->dstQueueFamilyIndex             = vk.graphics_queue_family;
        b->image                           = e->image;
        b->subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b->subresourceRange.baseMipLevel   = e->mip;
        b->subresourceRange.levelCount     = 1;
        b->subresourceRange.baseArrayLayer = e->layer;
        b->subresourceRange.layerCount     = 1;
    }

    /* Build buffer acquire barriers. */
    static VkBufferMemoryBarrier2 buf_bars[ VK_MAX_UPLOAD_ACQUIRES ];
    for ( u32 i = 0; i < g_pending_buffer_count; ++i )
    {
        VkBufferMemoryBarrier2* b = &buf_bars[ i ];

        *b = (VkBufferMemoryBarrier2){ 0 };
        b->sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b->srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
        b->srcAccessMask       = 0;
        b->dstStageMask        = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
                               | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT
                               | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                               | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                               | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b->dstAccessMask       = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
                               | VK_ACCESS_2_INDEX_READ_BIT
                               | VK_ACCESS_2_SHADER_READ_BIT;
        b->srcQueueFamilyIndex = vk.transfer_queue_family;
        b->dstQueueFamilyIndex = vk.graphics_queue_family;
        b->buffer              = g_pending_buffer_acquires[ i ];
        b->offset              = 0;
        b->size                = VK_WHOLE_SIZE;
    }

    VkDependencyInfo dep             = { 0 };
    dep.sType                        = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount      = g_pending_image_count;
    dep.pImageMemoryBarriers         = img_bars;
    dep.bufferMemoryBarrierCount     = g_pending_buffer_count;
    dep.pBufferMemoryBarriers        = buf_bars;
    vkCmdPipelineBarrier2( cmd, &dep );

    /* Track which contexts have consumed this batch.  The pending lists are global; only
       clear them after every allocated context has injected its acquire barriers, so each
       context's command buffer gets the full set regardless of call order.  acquire_ack_mask
       resets to 0 when cleared, ready for the next epoch's uploads. */
    vk.acquire_ack_mask |= ( 1u << ctx_id );
    if ( vk.acquire_ack_mask == vk.ctx_alloc )
    {
        g_pending_image_count  = 0;
        g_pending_buffer_count = 0;
        vk.acquire_ack_mask    = 0;
    }
}

/*============================================================================================*/
