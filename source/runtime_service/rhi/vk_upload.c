/*==============================================================================================

    vulkan/vk_upload.c -- Staged upload ring buffer for GPU-only resources.

    Simple idea: the GPU's fast memory (VRAM) cannot be written directly by the CPU.
    To load a texture or vertex buffer onto the GPU we use a "staging" buffer -- a temporary
    holding area that both the CPU and GPU can access.

      1. CPU copies data into the staging buffer (fast memcpy; like filling a bucket).
      2. GPU reads from staging and copies into VRAM via a DMA transfer command.
      3. Rendering waits for the copy to finish before shaders sample the new data.

    This file manages that pipeline so uploads from one frame do not overwrite transfers
    still in flight from a prior frame.

    --- Ring buffer ---

    We keep VK_MAX_FRAMES_IN_FLIGHT (2) staging slots, each 64 MB, persistently mapped so
    the CPU can memcpy into them at any time without extra setup.  Only one slot is "active"
    at a time (g_upload_active_slot).  vk_upload_flush() closes the current slot, submits
    its copy commands, and advances to the next.  Because slots rotate every
    VK_MAX_FRAMES_IN_FLIGHT flushes, an upload queued in flush G cannot be overwritten until
    flush G+VK_MAX_FRAMES_IN_FLIGHT -- by which point the GPU is long since done with it.

    Before writing into a recycled slot, vk_staging_alloc confirms the GPU finished reading
    it by checking the timeline semaphore value recorded at the time the slot was last
    submitted.  In practice this check is already satisfied by the frame-begin fence wait;
    the semaphore check is a safety net that survives future topology changes.

    --- Synchronization: the timeline semaphore ---

    A timeline semaphore is just a 64-bit counter both the CPU and GPU can observe.  One
    counter (upload_timeline) serves as the thread connecting transfer and graphics queues:

      * vk_upload_flush()    signals upload_timeline = ++upload_counter when DMA finishes.
      * vk_frame_end()       tells the graphics queue: stall vertex input and shaders until
                             upload_timeline reaches upload_counter.
      * vk_staging_alloc()   CPU-side: wait on last_submit_value before overwriting a slot.

    This replaces what once required a separate fence (for staging reuse safety) plus a
    separate binary semaphore (for GPU-GPU sync).  The same counter serves both purposes.
    Frames with no uploads skip the graphics-side wait entirely.

    --- Queue-family ownership transfers (QFOT) ---

    On discrete GPUs, the transfer queue and graphics queue are separate hardware units.
    A Vulkan resource used by both must formally change "owner" between them:

      Release barrier (in transfer command buffer):  "I am done; here is the intended layout."
      Acquire barrier (in graphics command buffer):  "I accept ownership; make it visible."

    The timeline semaphore provides the cross-queue execution dependency (DMA finishes before
    shaders run).  The acquire barrier provides the memory visibility guarantee on the
    graphics queue.  vk_upload_apply_acquires() injects the acquire side at the top of every
    graphics command buffer so any resource uploaded this epoch is ready before the first draw.

    On integrated GPUs where transfer and graphics share a queue family, QFOTs are skipped
    entirely and the layout transition is done inline in the transfer command buffer.

    --- Flow summary ---

    CPU                         Transfer Queue              Graphics Queue
    ---                         --------------              --------------
    memcpy to staging[ 0 ]
    record vkCmdCopy
    vk_upload_flush()
      submit to transfer ──────► DMA copy runs
      upload_counter = 1         signal upload_timeline = 1
                                                            frame_begin records commands
                                                            vk_upload_apply_acquires()
                                                            frame_end submit:
                                                              wait image_available_sem
                                                              wait upload_timeline = 1
                                                            ◄── semaphore fires
                                                              run shaders with new data
                                                              signal render_finished_sem
                                                              signal in_flight_fence

    -- next frame --
    frame_begin
      vkWaitForFences ◄─────────────────────────────────── in_flight_fence fires
      vk_staging_alloc on slot 0:
        vkWaitSemaphores( last_submit_value = 1 ) <- already satisfied, returns instantly
      memcpy overwrites staging[ 0 ] safely

==============================================================================================*/
// clang-format off

/* Per-slot command pool and recording state for the transfer queue.
   Lives here rather than in vk_staging_t to keep vk_state.c self-contained. */

typedef struct vk_upload_slot_s
{
    // Command pool for the transfer queue family.
    // One per slot so recording and submission can overlap across slots.
    VkCommandPool   pool;

    // Primary command buffer allocated from that pool.
    // Reset and re-recorded each cycle.
    VkCommandBuffer cmd;

    // True while the command buffer is open for recording.
    // Prevents double-begin if multiple uploads land in the same slot.
    bool            is_recording;

} vk_upload_slot_t;

/* One vk_upload_slot_t per frame-in-flight slot; g_upload_active_slot selects the active one. */

static vk_upload_slot_t g_upload[ VK_MAX_FRAMES_IN_FLIGHT ];
static u32              g_upload_active_slot = 0;

/* Pending queue-family ownership transfer acquires.
   Populated by vk_upload_texture / vk_upload_buffer when a dedicated transfer queue is in use.
   Drained by vk_upload_apply_acquires() at the top of each graphics command buffer.
   Global (not per-slot) so acquires are not lost when a frame_begin returns early. */

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
    vk_upload_init -- allocate staging buffers, command infrastructure, and the timeline sem.

    Creates the three things needed to upload data every frame:
      A. Staging buffers  -- host-visible memory the CPU memcpy's into directly.
      B. Transfer command pools / buffers  -- one per slot; records the GPU copy commands.
      C. Timeline semaphore  -- the 64-bit counter that syncs transfer and graphics queues.
==============================================================================================*/

static void vk_upload_shutdown( void );

static bool
vk_upload_init( void )
{
    /* Two staging slots (VK_MAX_FRAMES_IN_FLIGHT = 2), each 64 MB.  Persistently mapped so
       the CPU can write into them at any time without calling vkMapMemory each frame. */

    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        /* --- A. Staging buffer (CPU-to-GPU bridge) --- */

        VkBufferCreateInfo buf_ci = { 0 };
        buf_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size        = VK_STAGING_SIZE;
        buf_ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult r = vkCreateBuffer( vk.device, &buf_ci, vk.alloc_cb, &vk.staging[ i ].buffer );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkCreateBuffer[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }

        VkMemoryRequirements reqs;
        vkGetBufferMemoryRequirements( vk.device, vk.staging[ i ].buffer, &reqs );

        /* HOST_VISIBLE: CPU can write to it.
           HOST_COHERENT: writes are visible to the GPU immediately after; no manual flush needed. */

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

        /* Persistent map: keep the CPU pointer alive for the engine's lifetime to avoid the
           overhead of mapping and unmapping every frame. */

        r = vkMapMemory( vk.device, vk.staging[ i ].memory, 0, VK_STAGING_SIZE, 0, &vk.staging[ i ].mapped );
        if ( r != VK_SUCCESS )
        {
             LOG_ERROR( "upload_init: vkMapMemory[%u]: %s", i, string_VkResult( r ) );
             goto fail;
        }
        vk.staging[ i ].head = 0;

        /* --- B. Transfer command infrastructure --- */

        /* One command pool per slot on the transfer queue family.  Falls back to the graphics
           family on hardware without a dedicated transfer engine; in that case
           transfer_queue_family == graphics_queue_family and no QFOTs are needed. */

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

        /* One primary command buffer per slot; records vkCmdCopyBuffer / vkCmdCopyBufferToImage
           calls.  Reset and re-recorded each cycle. */

        r = vkAllocateCommandBuffers( vk.device, &alloc_info, &g_upload[ i ].cmd );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "upload_init: vkAllocateCommandBuffers[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }

        g_upload[ i ].is_recording = false;
    }

    /* --- C. Timeline semaphore --- */
    {
        /* Unlike a binary semaphore (just 0 or 1), a timeline semaphore is a 64-bit counter
           that both the CPU and GPU can read.  upload_counter on the CPU and upload_timeline
           on the GPU always hold the same value; the transfer queue increments both together
           each flush so either side can compare them. */

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

/*============================================================================================*/

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
    vk_staging_alloc -- allocate space in the active staging slot for an upload.
==============================================================================================*/

typedef struct vk_staging_alloc_s
{
    void*         cpu_ptr;      // where the caller should memcpy the data
    VkBuffer      buffer;       // the staging VkBuffer to pass to vkCmdCopy*
    VkDeviceSize  offset;       // byte offset of the allocation within buffer

} vk_staging_alloc_t;

/* Open the active slot's command buffer for recording if not already open. */
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

    /* First write into a recycled slot: confirm the GPU finished the prior DMA read from
       this buffer before the CPU overwrites it.  The epoch-gated flush in vk_frame.c keeps
       slots cycling at display-frame cadence, so the frame-begin fence wait normally already
       satisfies this semaphore.  The explicit check here is a belt-and-suspenders guarantee
       that stays correct regardless of future changes to context topology or flush ordering. */

    if ( s->head == 0 && s->last_submit_value > 0 )
    {
        VkSemaphoreWaitInfo wi = { 0 };
        wi.sType               = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount      = 1;
        wi.pSemaphores         = &vk.upload_timeline;
        wi.pValues             = &s->last_submit_value;
        vkWaitSemaphores( vk.device, &wi, UINT64_MAX );
    }

    /* Align the bump pointer and check that the allocation fits. */
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

    /* Ensure the command buffer is open so the caller can record the copy commands. */
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

    /* Carve out space in the staging ring and copy the data into it. */
    vk_staging_alloc_t sa;
    if ( !vk_staging_alloc( size, 4, &sa ) )
        return false;

    memcpy( sa.cpu_ptr, data, size );

    u32             slot    = g_upload_active_slot;
    VkCommandBuffer cmd     = g_upload[ slot ].cmd;
    VkBuffer        dst_buf = vk.buffers[ dst.id ].buffer;

    /* Record the GPU copy: staging -> destination buffer. */
    VkBufferCopy region = { 0 };
    region.srcOffset    = sa.offset;
    region.dstOffset    = 0;
    region.size         = size;
    vkCmdCopyBuffer( cmd, sa.buffer, dst_buf, 1, &region );

    if ( vk.transfer_queue_family != vk.graphics_queue_family )
    {
        /* Dedicated transfer queue: release ownership to the graphics family.
           The release barrier relinquishes ownership after the copy and declares the
           intended access pattern; the matching acquire in vk_upload_apply_acquires
           completes the handshake on the graphics queue side. */

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

    /* Carve out space in the staging ring and copy the pixel data into it. */
    vk_staging_alloc_t sa;
    if ( !vk_staging_alloc( data_size, 16, &sa ) )
        return false;

    memcpy( sa.cpu_ptr, data, data_size );

    u32                slot = g_upload_active_slot;
    VkCommandBuffer    cmd  = g_upload[ slot ].cmd;
    vk_texture_slot_t* tex  = &vk.textures[ dst.id ];

    u32 mip_w = tex->width  >> mip; if ( mip_w < 1 ) mip_w = 1;
    u32 mip_h = tex->height >> mip; if ( mip_h < 1 ) mip_h = 1;

    /* Transition the image from UNDEFINED to TRANSFER_DST_OPTIMAL so the GPU can write
       into it.  UNDEFINED discards whatever was there before, which is correct since we
       are about to overwrite the entire mip/layer. */

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

    /* Copy the pixel data from the staging buffer into the image. */
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
        /* Dedicated transfer queue: release ownership to the graphics family and declare
           the intended final layout (SHADER_READ_ONLY_OPTIMAL).  The acquire barrier in
           vk_upload_apply_acquires uses the same layout pair to complete the transition
           on the graphics queue, where shaders will read the image. */

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
        /* Same queue family (integrated GPU): no ownership transfer needed.  Perform the
           full layout transition here in the transfer command buffer.
           TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL. */

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
    vk_upload_flush -- submit the active slot's copy commands and advance to the next slot.

    Called from vk_frame.c at the top of frame_begin after the fence wait, at most once per
    display epoch.  If nothing was recorded this cycle (is_recording == false), the slot is
    still advanced so the ring keeps cycling at display-frame cadence.
==============================================================================================*/

static void
vk_upload_flush( void )
{
    u32                slot = g_upload_active_slot;
    vk_upload_slot_t*  up   = &g_upload[ slot ];

    if ( up->is_recording )
    {
        /* Close the command buffer and submit it to the transfer queue.
           Signal upload_timeline at ++upload_counter when the DMA batch completes.
           The graphics queue waits on that value in frame_end before running shaders. */

        vkEndCommandBuffer( up->cmd );

        VkCommandBufferSubmitInfo cmd_si = { 0 };
        cmd_si.sType                     = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_si.commandBuffer             = up->cmd;

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
            /* Record the signaled value on both the CPU counter and the slot so that
               vk_staging_alloc can check it before recycling this slot next cycle. */
            vk.upload_counter                    = signal_value;
            vk.staging[ slot ].last_submit_value = signal_value;
        }

        vkResetCommandBuffer( up->cmd, 0 );
        up->is_recording = false;
    }

    /* Reset the bump allocator for this slot and advance to the next slot. */
    vk.staging[ slot ].head  = 0;
    g_upload_active_slot = ( slot + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
}

/*==============================================================================================
    vk_upload_apply_acquires -- inject QFOT acquire barriers into a graphics command buffer.

    Called at the top of every graphics command buffer in vk_frame_begin, before any draws.
    Completes the two-step ownership handshake for resources uploaded on the transfer queue.

    The pending lists are global and cleared only after every active context has called this
    function, so each context's command buffer receives the full set of acquire barriers
    regardless of which context calls frame_begin first.
==============================================================================================*/

static void
vk_upload_apply_acquires( VkCommandBuffer cmd, i32 ctx_id )
{
    /* Same-family path: no QFOTs were issued by vk_upload_texture / vk_upload_buffer. */
    if ( vk.transfer_queue_family == vk.graphics_queue_family )
        return;
    if ( g_pending_image_count == 0 && g_pending_buffer_count == 0 )
        return;

    /* Build image acquire barriers.
       The matching release in vk_upload_texture declared TRANSFER_DST -> SHADER_READ_ONLY;
       the acquire must use the same layout pair to complete the transition.
       srcStageMask = NONE because the timeline semaphore wait in the graphics submit already
       provides the inter-queue execution dependency; the acquire only needs to declare the
       memory visibility on the graphics side. */

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

    /* Mark this context as done.  The pending lists are only cleared once every active
       context has injected its acquire barriers, ensuring no context misses a resource
       that was uploaded before its frame_begin ran.  acquire_ack_mask resets to 0 when
       cleared, ready for the next epoch's uploads. */

    vk.acquire_ack_mask |= ( 1u << ctx_id );
    if ( vk.acquire_ack_mask == vk.ctx_alloc )
    {
        g_pending_image_count  = 0;
        g_pending_buffer_count = 0;
        vk.acquire_ack_mask    = 0;
    }
}

/*============================================================================================*/
// clang-format on
