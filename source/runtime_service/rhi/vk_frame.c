/*==============================================================================================

    vulkan/vk_frame.c -- Per-frame orchestration.

    frame_begin and frame_end drive per-window Vulkan choreography.  Each context
    advances its own current_frame counter independently.  frame_begin returns an open
    command list with no active render pass; callers use cmd_begin_rendering /
    cmd_end_rendering to open and close passes explicitly.

    Uses VK 1.3 dynamic rendering.  No VkRenderPass or VkFramebuffer objects are created.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Frame begin / end
==============================================================================================*/

static rhi_command_list_t
vk_frame_begin( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return RHI_CMD_INVALID;

    /* Handle deferred resize (never mid-recording; always between frames). */
    if ( ctx->resize_pending )
    {
        /* vk_swapchain_recreate calls vkDeviceWaitIdle, so the GPU is idle when it
           returns.  Recreate sync objects too: semaphores that were consumed by the
           old present's vkQueuePresentKHR wait are tracked by the validation layer as
           associated with the old swapchain, and re-signaling them on the new swapchain
           triggers a spurious hazard warning.  Fresh handles have no WSI history. */
        vk_swapchain_recreate( ctx );
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

    /* Flush staged uploads from the previous cycle; advances g_upload_active_slot. */
    vk_upload_flush();

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

    /* Depth image: UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL (first frame only). */
    if ( ctx->depth_layout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL )
    {
        VkImageMemoryBarrier2* depth_b               = &barriers[ barrier_count++ ];
        depth_b->sType                               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        depth_b->srcStageMask                        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        depth_b->srcAccessMask                       = 0;
        depth_b->dstStageMask                        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        depth_b->dstAccessMask                       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth_b->oldLayout                           = ctx->depth_layout;
        depth_b->newLayout                           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_b->srcQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
        depth_b->dstQueueFamilyIndex                 = VK_QUEUE_FAMILY_IGNORED;
        depth_b->image                               = ctx->depth_image;
        depth_b->subresourceRange.aspectMask         = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_b->subresourceRange.baseMipLevel       = 0;
        depth_b->subresourceRange.levelCount         = 1;
        depth_b->subresourceRange.baseArrayLayer     = 0;
        depth_b->subresourceRange.layerCount         = 1;

        ctx->depth_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    VkDependencyInfo dep_info        = { 0 };
    dep_info.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.imageMemoryBarrierCount = barrier_count;
    dep_info.pImageMemoryBarriers    = barriers;

    vkCmdPipelineBarrier2( cmd_buf, &dep_info );

    /* Wire the command list slot so vk_cmd_from_handle resolves this frame correctly. */
    ctx->cmd_lists[ frame ].vk_cmd = cmd_buf;
    ctx->cmd_lists[ frame ].ctx_id = ctx_id;
    ctx->cmd_lists[ frame ].frame  = frame;

    return vk_cmd_make_handle( ctx_id, frame );
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
    VkSemaphoreSubmitInfo wait_sem    = { 0 };
    wait_sem.sType                    = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_sem.semaphore                = ctx->image_available_sem[ frame ];
    wait_sem.stageMask                = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal_sem  = { 0 };
    signal_sem.sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_sem.semaphore              = ctx->render_finished_sem[ ctx->image_index ];
    signal_sem.stageMask              = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmd_si  = { 0 };
    cmd_si.sType                      = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_si.commandBuffer              = cmd_buf;

    VkSubmitInfo2 submit              = { 0 };
    submit.sType                      = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount     = 1;
    submit.pWaitSemaphoreInfos        = &wait_sem;
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

/*==============================================================================================
    Render pass open / close
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

static void
vk_cmd_begin_rendering( rhi_command_list_t             cmd,
                        const rhi_color_attachment_t*  color_atts, u32 color_count,
                        const rhi_depth_attachment_t*  depth_att )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl )
        return;

    vk_context_t* ctx = vk_ctx_get( cl->ctx_id );
    if ( !ctx )
        return;

    /* Build color attachment infos. */
    VkRenderingAttachmentInfo color_infos[ RHI_MAX_COLOR_TARGETS ] = { 0 };
    u32 cc = ( color_count < RHI_MAX_COLOR_TARGETS ) ? color_count : RHI_MAX_COLOR_TARGETS;
    for ( u32 i = 0; i < cc; i++ )
    {
        const rhi_color_attachment_t* ca = &color_atts[ i ];
        VkRenderingAttachmentInfo*    ai = &color_infos[ i ];

        ai->sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ai->imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ai->loadOp      = vk_load_op( ca->load_op );
        ai->storeOp     = vk_store_op( ca->store_op );
        ai->imageView   = ( ca->texture.id == RHI_SWAPCHAIN_COLOR )
                          ? ctx->swapchain_image_views[ ctx->image_index ]
                          : vk.textures[ ca->texture.id ].view;

        if ( ca->load_op == RHI_LOAD_OP_CLEAR )
        {
            ai->clearValue.color.float32[ 0 ] = ca->clear.r;
            ai->clearValue.color.float32[ 1 ] = ca->clear.g;
            ai->clearValue.color.float32[ 2 ] = ca->clear.b;
            ai->clearValue.color.float32[ 3 ] = ca->clear.a;
        }
    }

    /* Build depth attachment info. */
    VkRenderingAttachmentInfo depth_info = { 0 };
    if ( depth_att )
    {
        depth_info.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_info.loadOp      = vk_load_op( depth_att->load_op );
        depth_info.storeOp     = vk_store_op( depth_att->store_op );
        depth_info.imageView   = ( depth_att->texture.id == RHI_SWAPCHAIN_DEPTH )
                                 ? ctx->depth_view
                                 : vk.textures[ depth_att->texture.id ].view;

        if ( depth_att->load_op == RHI_LOAD_OP_CLEAR )
        {
            depth_info.clearValue.depthStencil.depth   = depth_att->depth_clear;
            depth_info.clearValue.depthStencil.stencil = depth_att->stencil_clear;
        }
    }

    VkRenderingInfo ri               = { 0 };
    ri.sType                         = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset             = (VkOffset2D){ 0, 0 };
    ri.renderArea.extent             = ctx->swapchain_extent;
    ri.layerCount                    = 1;
    ri.colorAttachmentCount          = cc;
    ri.pColorAttachments             = cc > 0 ? color_infos : NULL;
    ri.pDepthAttachment              = depth_att ? &depth_info : NULL;

    vkCmdBeginRendering( cl->vk_cmd, &ri );
}

static void
vk_cmd_end_rendering( rhi_command_list_t cmd )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl )
        return;

    vkCmdEndRendering( cl->vk_cmd );
}

/*==============================================================================================
    Commands  (v0 -- expanded as each subsystem comes online)
==============================================================================================*/

static void
vk_cmd_set_viewport( rhi_command_list_t cmd, const rhi_viewport_t* vp )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !vp )
        return;

    VkViewport vkvp;
    vkvp.x        = vp->x;
    vkvp.y        = vp->y;
    vkvp.width    = vp->width;
    vkvp.height   = vp->height;
    vkvp.minDepth = vp->min_depth;
    vkvp.maxDepth = vp->max_depth;
    vkCmdSetViewport( cl->vk_cmd, 0, 1, &vkvp );
}

static void
vk_cmd_set_scissor( rhi_command_list_t cmd, const rhi_rect_t* rect )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !rect )
        return;

    VkRect2D sc;
    sc.offset.x      = rect->x;
    sc.offset.y      = rect->y;
    sc.extent.width  = (u32)rect->width;
    sc.extent.height = (u32)rect->height;
    vkCmdSetScissor( cl->vk_cmd, 0, 1, &sc );
}

static void
vk_cmd_bind_pipeline( rhi_command_list_t cmd, rhi_pipeline_t pipeline )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !vk_pipeline_validate( pipeline ) )
        return;

    VkPipeline vkp = vk.pipelines[ pipeline.id ].pipeline;
    vkCmdBindPipeline( cl->vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp );
}

static void
vk_cmd_bind_vertex_buffer( rhi_command_list_t cmd, rhi_buffer_t buf, u32 offset )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !vk_buffer_validate( buf ) )
        return;

    VkBuffer     vkb = vk.buffers[ buf.id ].buffer;
    VkDeviceSize off = offset;
    vkCmdBindVertexBuffers( cl->vk_cmd, 0, 1, &vkb, &off );
}

static void
vk_cmd_bind_index_buffer( rhi_command_list_t cmd, rhi_buffer_t buf, u32 offset,
                           rhi_index_type_t type )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !vk_buffer_validate( buf ) )
        return;

    VkBuffer    vkb  = vk.buffers[ buf.id ].buffer;
    VkIndexType vkt  = ( type == RHI_INDEX_TYPE_UINT16 ) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer( cl->vk_cmd, vkb, offset, vkt );
}

static void
vk_cmd_push_constants( rhi_command_list_t cmd, const void* data, u32 size, u32 offset )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !data || size == 0 )
        return;

    vkCmdPushConstants( cl->vk_cmd, vk.pipeline_layout,
                        VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
                        offset, size, data );
}

static void
vk_cmd_draw( rhi_command_list_t cmd, const rhi_draw_args_t* args )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !args )
        return;

    vkCmdDraw( cl->vk_cmd, args->vertex_count, args->instance_count,
               args->first_vertex, args->first_instance );
}

static void
vk_cmd_draw_indexed( rhi_command_list_t cmd, const rhi_draw_indexed_args_t* args )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl || !args )
        return;

    vkCmdDrawIndexed( cl->vk_cmd, args->index_count, args->instance_count,
                      args->first_index, args->vertex_offset, args->first_instance );
}

static void
vk_cmd_bind_bindless( rhi_command_list_t cmd )
{
    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !cl )
        return;

    /* Bind for both points: descriptor set state is per-bind-point in Vulkan, and the
       pipeline layout covers compute (push constant range includes COMPUTE_BIT). */
    vkCmdBindDescriptorSets( cl->vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             vk.pipeline_layout, 0, 1, &vk.bindless_set, 0, NULL );
    vkCmdBindDescriptorSets( cl->vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             vk.pipeline_layout, 0, 1, &vk.bindless_set, 0, NULL );
}

/*============================================================================================*/
// clang-format on
