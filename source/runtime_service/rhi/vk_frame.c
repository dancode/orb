/*==============================================================================================

    vulkan/vk_frame.c -- Per-frame GPU choreography: begin and end a frame.

    vk_frame_begin  --  fence wait, epoch check-in, upload flush, swapchain acquire,
                        QFOT acquires, layout transitions, command buffer open.
    vk_frame_end    --  present barrier, queue submit (sync2), swapchain present.

    Each context advances its own current_frame counter independently.  frame_begin
    returns an open command list with no active render pass; callers use
    vk_cmd_begin_rendering / vk_cmd_end_rendering (in vk_cmd_graphics.c) to open
    and close passes explicitly.

    Uses VK 1.3 dynamic rendering.  No VkRenderPass or VkFramebuffer objects are created.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Frame begin / end
==============================================================================================*/

static rhi_cmd_list_t
vk_frame_begin( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return RHI_CMD_INVALID;

    /* Handle deferred resize (never mid-recording; always between frames). */
    if ( ctx->resize_pending )
    {
        /* vk_swapchain_recreate waits on this context's fences and the present queue,
           so this context's GPU work is idle when it returns.  Recreate sync objects
           too: semaphores consumed by the old present's vkQueuePresentKHR wait are
           tracked by the validation layer as associated with the old swapchain, and
           re-signaling them on the new swapchain triggers a spurious hazard warning.
           Fresh handles have no WSI history.
           Returns false when the window is minimized (surface extent {0,0}); in that
           case leave resize_pending set and skip the frame -- retry next pump. */
        if ( !vk_swapchain_recreate( ctx ) )
            return RHI_CMD_INVALID;
        vk_sync_destroy( ctx );
        vk_sync_create( ctx );
        ctx->resize_pending = false;
    }

    /* Advance the global frame counter (monotonic; for diagnostics / future use). */
    vk.global_frame++;

    u32             frame   = ctx->current_frame;
    VkCommandBuffer cmd_buf = ctx->command_buffers[ frame ];

    /* Block until this slot's previous GPU work is complete. */
    VkResult r = vkWaitForFences( vk.device, 1, &ctx->in_flight_fence[ frame ], VK_TRUE, UINT64_MAX );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "frame_begin: vkWaitForFences: %s", string_VkResult( r ) );
        return RHI_CMD_INVALID;
    }

    /* Epoch check-in: this context has confirmed its fence.  When every active context
       has checked in the epoch advances, allowing deferred descriptor slots to be retired. */
    vk.epoch_ack_mask |= ( 1u << ctx_id );
    if ( vk.epoch_ack_mask == vk.ctx_alloc )
    {
        vk.global_epoch++;
        vk.epoch_ack_mask = 0;
    }

    /* Flush staged uploads once per display epoch regardless of how many contexts call
       frame_begin.  upload_flush_epoch starts at 0 and global_epoch at 1, so the first
       context of every epoch (including the very first frame) triggers the flush; every
       subsequent context in that same epoch skips it, preventing the upload slot from
       cycling faster than once per display frame. */
    if ( vk.upload_flush_epoch < vk.global_epoch )
    {
        vk.upload_flush_epoch = vk.global_epoch;
        vk_upload_flush();
    }

    /* Return deferred bindless slots whose GPU references have expired. */
    vk_descriptor_flush_retired();

    /* Acquire the next presentable swapchain image. */
    r = vkAcquireNextImageKHR( vk.device, ctx->swapchain, UINT64_MAX,
                                ctx->image_available_sem[ frame ], VK_NULL_HANDLE,
                                &ctx->image_index );
    if ( r == VK_ERROR_OUT_OF_DATE_KHR )
    {
        ctx->resize_pending = true;
        return RHI_CMD_INVALID;
    }
    if ( r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR )
    {
        LOG_ERROR( "frame_begin: vkAcquireNextImageKHR: %s", string_VkResult( r ) );
        return RHI_CMD_INVALID;
    }

    /* Reset fence only after we know we will submit work this frame. */
    vkResetFences( vk.device, 1, &ctx->in_flight_fence[ frame ] );

    /* Reset and begin the command buffer. */
    vkResetCommandBuffer( cmd_buf, 0 );

    VkCommandBufferBeginInfo begin_info = { 0 };
    begin_info.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    r = vkBeginCommandBuffer( cmd_buf, &begin_info );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "frame_begin: vkBeginCommandBuffer: %s", string_VkResult( r ) );
        return RHI_CMD_INVALID;
    }

    /* Acquire any resources that were uploaded on the transfer queue this cycle.
       On hardware with a dedicated transfer family, images and buffers uploaded via
       vk_upload_texture/vk_upload_buffer need a QFOT acquire barrier here before
       any draw that samples them.  On integrated GPUs (same family) this is a no-op. */
    vk_upload_apply_acquires( cmd_buf, ctx_id );

    /* Build the barrier array.  The color barrier is always issued (UNDEFINED srcLayout
       is valid per spec -- contents are discarded, which is fine with loadOp=CLEAR).
       The depth barrier only fires on first use; after that depth stays in
       DEPTH_ATTACHMENT_OPTIMAL between frames so no barrier is needed. */

    VkImageMemoryBarrier2 barriers[ 2 ] = { 0 };
    u32 barrier_count = 0;

    /* Swapchain color image: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL */
    VkImageMemoryBarrier2* color_b                   = &barriers[ barrier_count++ ];
    color_b->sType                                   = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    color_b->srcStageMask                            = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    color_b->srcAccessMask                           = 0;
    color_b->dstStageMask                            = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    color_b->dstAccessMask                           = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    color_b->oldLayout                               = VK_IMAGE_LAYOUT_UNDEFINED;
    color_b->newLayout                               = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_b->srcQueueFamilyIndex                     = VK_QUEUE_FAMILY_IGNORED;
    color_b->dstQueueFamilyIndex                     = VK_QUEUE_FAMILY_IGNORED;
    color_b->image                                   = ctx->swapchain_images[ ctx->image_index ];
    color_b->subresourceRange.aspectMask             = VK_IMAGE_ASPECT_COLOR_BIT;
    color_b->subresourceRange.baseMipLevel           = 0;
    color_b->subresourceRange.levelCount             = 1;
    color_b->subresourceRange.baseArrayLayer         = 0;
    color_b->subresourceRange.layerCount             = 1;

    /* Depth image: UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL (first use of this slot only).
       The fence wait above guarantees the previous use of this frame slot is complete,
       so depth_layout[frame] accurately reflects the image state for this slot. */
    if ( ctx->depth_layout[ frame ] != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL )
    {
        VkImageMemoryBarrier2* depth_b               = &barriers[ barrier_count++ ];
        depth_b->sType                               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        depth_b->srcStageMask                        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        depth_b->srcAccessMask                       = 0;
        depth_b->dstStageMask                        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        depth_b->dstAccessMask                       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth_b->oldLayout                           = ctx->depth_layout[ frame ];
        depth_b->newLayout                           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_b->srcQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
        depth_b->dstQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
        depth_b->image                               = ctx->depth_image[ frame ];
        depth_b->subresourceRange.aspectMask         = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_b->subresourceRange.baseMipLevel       = 0;
        depth_b->subresourceRange.levelCount         = 1;
        depth_b->subresourceRange.baseArrayLayer     = 0;
        depth_b->subresourceRange.layerCount         = 1;

        ctx->depth_layout[ frame ] = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    VkDependencyInfo dep_info        = { 0 };
    dep_info.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.imageMemoryBarrierCount = barrier_count;
    dep_info.pImageMemoryBarriers    = barriers;

    vkCmdPipelineBarrier2( cmd_buf, &dep_info );

    /* Initialize the command list slot and return a direct pointer to it. */
    ctx->cmd_lists[ frame ].vk_cmd = cmd_buf;
    ctx->cmd_lists[ frame ].ctx_id = ctx_id;
    ctx->cmd_lists[ frame ].frame  = frame;

    return &ctx->cmd_lists[ frame ];
}

static void
vk_frame_end( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return;

    u32             frame   = ctx->current_frame;
    VkCommandBuffer cmd_buf = ctx->command_buffers[ frame ];

    /* Barrier: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR. */
    VkImageMemoryBarrier2 present_b               = { 0 };
    present_b.sType                               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    present_b.srcStageMask                        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    present_b.srcAccessMask                       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    present_b.dstStageMask                        = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    present_b.dstAccessMask                       = 0;
    present_b.oldLayout                           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    present_b.newLayout                           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    present_b.srcQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
    present_b.dstQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
    present_b.image                               = ctx->swapchain_images[ ctx->image_index ];
    present_b.subresourceRange.aspectMask         = VK_IMAGE_ASPECT_COLOR_BIT;
    present_b.subresourceRange.baseMipLevel       = 0;
    present_b.subresourceRange.levelCount         = 1;
    present_b.subresourceRange.baseArrayLayer     = 0;
    present_b.subresourceRange.layerCount         = 1;

    VkDependencyInfo dep_info        = { 0 };
    dep_info.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.imageMemoryBarrierCount = 1;
    dep_info.pImageMemoryBarriers    = &present_b;

    vkCmdPipelineBarrier2( cmd_buf, &dep_info );

    VkResult r = vkEndCommandBuffer( cmd_buf );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "frame_end: vkEndCommandBuffer: %s", string_VkResult( r ) );
        ctx->current_frame = ( ctx->current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
        return;
    }

    /* Submit via vkQueueSubmit2 (sync2 path). */

    /* Wait semaphores: swapchain image available + any pending upload batch. */
    VkSemaphoreSubmitInfo wait_sems[ 2 ] = { 0 };
    u32 wait_count = 0;

    wait_sems[ wait_count ].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_sems[ wait_count ].semaphore = ctx->image_available_sem[ frame ];
    wait_sems[ wait_count ].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    wait_count++;

    /* Stall vertex-input, shader, and compute stages until the DMA batch completes.
       Vertex input and index input are listed because uploaded buffers may be used as
       vertex/index data; the 3D front-end (draw setup, early-Z) can still overlap. */
    if ( vk.upload_counter > 0 )
    {
        wait_sems[ wait_count ].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_sems[ wait_count ].semaphore = vk.upload_timeline;
        wait_sems[ wait_count ].value     = vk.upload_counter;
        wait_sems[ wait_count ].stageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
                                          | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT
                                          | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        wait_count++;
    }

    VkSemaphoreSubmitInfo signal_sem  = { 0 };
    signal_sem.sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_sem.semaphore              = ctx->render_finished_sem[ ctx->image_index ];
    signal_sem.stageMask              = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmd_si  = { 0 };
    cmd_si.sType                      = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_si.commandBuffer              = cmd_buf;

    VkSubmitInfo2 submit              = { 0 };
    submit.sType                      = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount     = wait_count;
    submit.pWaitSemaphoreInfos        = wait_sems;
    submit.commandBufferInfoCount     = 1;
    submit.pCommandBufferInfos        = &cmd_si;
    submit.signalSemaphoreInfoCount   = 1;
    submit.pSignalSemaphoreInfos      = &signal_sem;

    r = vkQueueSubmit2( vk.graphics_queue, 1, &submit, ctx->in_flight_fence[ frame ] );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "frame_end: vkQueueSubmit2: %s", string_VkResult( r ) );
        ctx->current_frame = ( ctx->current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
        return;
    }

    /* Present. */
    VkPresentInfoKHR present_info   = { 0 };
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = &ctx->render_finished_sem[ ctx->image_index ];
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &ctx->swapchain;
    present_info.pImageIndices      = &ctx->image_index;

    r = vkQueuePresentKHR( vk.present_queue, &present_info );
    if ( r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR )
    {
        ctx->resize_pending = true;
    }
    else if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "frame_end: vkQueuePresentKHR: %s", string_VkResult( r ) );
    }

    ctx->current_frame = ( ctx->current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
}

/*============================================================================================*/
// clang-format on
