/*==============================================================================================

    vulkan/vk_frame.c -- Begin and end one rendered frame.

    Simple idea: every frame the engine draws one picture.

      vk_frame_begin  sets up a blank canvas: waits for the GPU to free the slot, acquires
                      a swapchain image, opens a command buffer, and issues any needed layout
                      transitions so the GPU can draw and sample correctly.
      vk_frame_end    finishes the picture: closes the command buffer, submits it to the GPU,
                      and hands the completed image to the display engine for presentation.

    The tricky part is timing.  The CPU runs ahead of the GPU -- it records commands for
    frame N while the GPU is still executing frame N-1.  Three sync objects keep order:

      in_flight_fence      CPU blocks here at frame_begin until the GPU finishes the prior
                           use of this slot.  Protects command buffers, staging memory, and
                           depth images from being overwritten while still in use.

      image_available_sem  GPU stalls COLOR_ATTACHMENT_OUTPUT until the display engine
                           releases the swapchain image.  Rarely fires in practice.

      render_finished_sem  GPU signals this when done.  vkQueuePresentKHR waits on it so
                           the display engine does not scan out an in-progress image.

    A fourth wait (upload_timeline) stalls vertex input and shader stages until any pending
    DMA uploads finish.  vk_upload.c owns that semaphore; frame_end adds the wait only when
    uploads were flushed this epoch (upload_counter > 0).

    Uses VK 1.3 dynamic rendering.  No VkRenderPass or VkFramebuffer objects are created.

==============================================================================================*/
// clang-format off

/*==============================================================================================

    vk_frame_begin -- begin a new frame on the given context.

    Steps performed in order:
      1. Handle any deferred swapchain resize.
      2. Wait for the GPU to finish the previous job in this frame slot (CPU fence wait).
      3. Epoch check-in: advance global_epoch when all contexts have checked in.
      4. Flush staged uploads (once per epoch, not once per context).
      5. Retire expired bindless descriptor slots.
      6. Acquire the next presentable swapchain image.
      7. Reset fence and command buffer.
      8. Inject QFOT acquire barriers for resources uploaded on the transfer queue.
      9. Issue layout transitions: color UNDEFINED->COLOR_ATTACHMENT_OPTIMAL each frame;
         depth UNDEFINED->DEPTH_ATTACHMENT_OPTIMAL on first use of each slot only.

    Returns an open command list with no active render pass.
    Returns RHI_CMD_INVALID on failure (swapchain out of date, fence error, etc.).

==============================================================================================*/

static rhi_cmd_t
vk_frame_begin( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return RHI_CMD_INVALID;

    /* --- Handle deferred resize. ---

       A resize is never applied mid-frame -- always deferred to the next frame_begin so we
       never rebuild the swapchain while the GPU is recording commands.

       vk_swapchain_recreate waits for this context's fences and the present queue, so all
       GPU work is idle when it returns.  It returns false when the window is minimized
       (surface extent {0,0}); in that case we leave resize_pending set and skip the frame,
       retrying on the next pump.

       Sync objects are recreated rather than reused: semaphores consumed by the old
       vkQueuePresentKHR are associated with the old swapchain in the validation layer, and
       re-signaling them on the new swapchain triggers a spurious hazard warning.  Fresh
       handles have no WSI history. */

    if ( ctx->resize_pending )
    {
        if ( !vk_swapchain_recreate( ctx ) )
            return RHI_CMD_INVALID;

        vk_sync_destroy( ctx );
        vk_sync_create( ctx );
        ctx->resize_pending = false;
    }

    u32             frame   = ctx->current_frame;
    VkCommandBuffer cmd_buf = ctx->command_buffers[ frame ];

    /* --- Step 1: Wait for the GPU to finish the previous job in this slot. ---

       Frame slots are reused every VK_MAX_FRAMES_IN_FLIGHT frames.  Before we can safely
       reset and record into this slot's command buffer -- or overwrite its staging memory
       or depth image -- we must confirm the GPU has finished reading them.
       This fence was signaled by vkQueueSubmit2 the last time this slot was used.
       UINT64_MAX means wait as long as needed; in practice it returns almost immediately. */

    VkResult r = vkWaitForFences( vk.device, 1, &ctx->in_flight_fence[ frame ], VK_TRUE, UINT64_MAX );
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "frame_begin: vkWaitForFences: %s", string_VkResult( r ) );
         return RHI_CMD_INVALID;
    }

    /* --- Step 2: Epoch check-in and upload flush. ---

       "Epoch" means one display frame.  The engine may have more than one context (window),
       and vk_upload_flush() must fire exactly once per display frame, not once per context.

       Each context sets its bit in epoch_ack_mask after its fence wait (proof that the GPU
       slot is idle).  When every active context has checked in, global_epoch advances and
       the mask resets.  upload_flush_epoch tracks when the flush last ran; if it lags behind
       global_epoch, this is the first context of the new epoch -- flush, then update.
       Every subsequent context in the same epoch skips the flush. */

    vk.epoch_ack_mask |= ( 1u << ctx_id );
    if ( vk.epoch_ack_mask == vk.ctx_alloc )
    {
        vk.global_epoch++;          // Every active context has checked in.
        vk.epoch_ack_mask = 0;      // Reset for the next epoch.
    }

    if ( vk.upload_flush_epoch < vk.global_epoch ) {
         vk.upload_flush_epoch = vk.global_epoch;
         vk_upload_flush();
    }

    /* Return deferred bindless slots whose GPU references have expired. */
    vk_descriptor_flush_retired();

    /* --- Step 3: Acquire the next swapchain image. ---

       The swapchain is a pool of images the GPU renders into and the display engine scans
       out to the monitor.  vkAcquireNextImageKHR gives us one that is currently free and
       stores its index in ctx->image_index.  It also arms image_available_sem[frame]: the
       GPU waits on that semaphore at COLOR_ATTACHMENT_OUTPUT in frame_end, ensuring it does
       not write color before the display engine is done reading the image for presentation.

       The fence is intentionally NOT reset before this call.  If acquisition fails (swapchain
       out of date), we return without submitting work -- and if we had already reset the
       fence, the next frame_begin would wait on it forever because nothing signals it.
       By deferring the reset to after a successful acquire, the fence stays signaled and the
       next frame_begin passes through immediately. */

    r = vkAcquireNextImageKHR( vk.device, ctx->swapchain, UINT64_MAX,
                                ctx->image_available_sem[ frame ], VK_NULL_HANDLE,
                                &ctx->image_index );

    if ( r == VK_ERROR_OUT_OF_DATE_KHR )
    {
        ctx->resize_pending = true;
        return RHI_CMD_INVALID;
    }

    /* VK_SUBOPTIMAL_KHR: the swapchain still works but no longer perfectly matches the
       surface (e.g. a window resize just started).  Render this frame and recreate next. */

    if ( r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR ) {
         LOG_ERROR( "frame_begin: vkAcquireNextImageKHR: %s", string_VkResult( r ) );
         return RHI_CMD_INVALID;
    }

    /* Reset fence only after we know we will submit work this frame. */
    vkResetFences( vk.device, 1, &ctx->in_flight_fence[ frame ] );

    /* Reset and begin recording the command buffer for this slot. */
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

    /* --- Step 4: Inject QFOT acquire barriers for any uploaded resources. ---

       On hardware with a dedicated transfer queue family, resources uploaded via
       vk_upload_texture / vk_upload_buffer were released by the transfer queue and need
       a matching acquire barrier here before any draw commands can sample them.
       The timeline semaphore wait in frame_end provides the cross-queue execution dependency;
       these barriers provide the memory visibility guarantee on the graphics queue side.
       On integrated GPUs (same queue family for transfer and graphics), this is a no-op. 
       
       Only one context needs to call vk_upload_apply_acquires() because the pending acquire 
       lists are global, but we don't know who is first, late comers are no-ops ops so its ok. */

    vk_upload_apply_acquires( cmd_buf, ctx_id );

    /* --- Step 5: Layout transitions. ---

       Vulkan images have a "layout" that tells the GPU how to read the underlying memory
       for a given purpose.  Using the wrong layout is undefined behavior.

       Color image (swapchain): UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL every frame.
         UNDEFINED tells the driver it may discard old contents, which is correct because
         loadOp=CLEAR will overwrite them anyway.  The barrier blocks color writes until
         the COLOR_ATTACHMENT_OUTPUT stage is reached.

       Depth image: UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL, first use of this slot only.
         After the first frame, the depth image stays in DEPTH_ATTACHMENT_OPTIMAL between
         frames so no barrier is needed.  The fence wait above guarantees the previous use
         of this slot is fully complete, so depth_layout[frame] accurately reflects the
         image's current state when we read it here.                                       */

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

    /* Depth image: UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL (first use of this slot only). */
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

/*==============================================================================================

    vk_frame_end -- submit recorded commands and present the swapchain image.

    Steps performed in order:
      1. Record a present barrier: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR.
      2. Close the command buffer.
      3. Submit to the graphics queue via vkQueueSubmit2 (sync2):
           - WAIT image_available_sem  (stall color output until display releases the image)
           - WAIT upload_timeline      (stall shaders until any DMA uploads finish)
           - SIGNAL render_finished_sem (fires when the GPU finishes all commands)
           - SIGNAL in_flight_fence     (fires when done; CPU waits on this next frame)
      4. Present the image via vkQueuePresentKHR.

==============================================================================================*/

static void
vk_frame_end( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return;

    u32             frame   = ctx->current_frame;
    VkCommandBuffer cmd_buf = ctx->command_buffers[ frame ];

    /* --- Step 1: Transition the swapchain image to PRESENT_SRC_KHR. ---

       The GPU just finished writing color in COLOR_ATTACHMENT_OPTIMAL layout.  The display
       engine needs the image in PRESENT_SRC_KHR layout before it can scan it out.
       srcStageMask = COLOR_ATTACHMENT_OUTPUT: declares that all color writes are finished.
       dstStageMask = BOTTOM_OF_PIPE:          the presentation engine reads after all GPU work. */

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

    /* --- Step 2: Build the wait semaphore list. ---

       The graphics queue cannot start certain stages until two conditions are met:
         (a) The display engine has released the swapchain image (image_available_sem).
             This stalls only COLOR_ATTACHMENT_OUTPUT -- all other stages run freely.
         (b) Any pending DMA uploads are complete (upload_timeline).
             This stalls vertex input, index input, and shader stages.

       Separating these waits lets the GPU overlap the 3D front-end stages (draw setup,
       early-Z) with in-flight DMA transfers on the dedicated transfer engine.            */

    VkSemaphoreSubmitInfo wait_sems[ 2 ] = { 0 };
    u32 wait_count = 0;

    /* Wait for the swapchain image to be released by the display engine. */
    wait_sems[ wait_count ].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_sems[ wait_count ].semaphore = ctx->image_available_sem[ frame ];
    wait_sems[ wait_count ].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    wait_count++;

    /* Wait for any in-flight DMA upload to finish before shaders sample the new data.
       Skipped entirely when no uploads were flushed this epoch. */

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

    /* Signal render_finished_sem when all GPU commands are done.
       vkQueuePresentKHR waits on this before handing the image to the display engine. */

    VkSemaphoreSubmitInfo signal_sem  = { 0 };
    signal_sem.sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_sem.semaphore              = ctx->render_finished_sem[ ctx->image_index ];
    signal_sem.stageMask              = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    /* One command buffer, one or two wait semaphores. */

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

    /* Submit work.  in_flight_fence is signaled when the GPU finishes this batch;
       the CPU waits on it at the start of the next use of this frame slot. */

    r = vkQueueSubmit2( vk.graphics_queue, 1, &submit, ctx->in_flight_fence[ frame ] );
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "frame_end: vkQueueSubmit2: %s", string_VkResult( r ) );
         ctx->current_frame = ( ctx->current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
         return;
    }

    /* --- Step 3: Hand the finished image to the display engine. ---

       vkQueuePresentKHR blocks until the display engine accepts the request (not until it
       actually scans out the image).  It waits on render_finished_sem internally so the GPU
       must finish writing before the display engine reads.
       OUT_OF_DATE or SUBOPTIMAL means the swapchain no longer matches the surface; defer
       recreation to the next frame_begin. */

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
