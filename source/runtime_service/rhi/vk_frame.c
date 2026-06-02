/*==============================================================================================

    vulkan/vk_frame.c -- Per-frame orchestration.

    frame_begin and frame_end drive per-window Vulkan choreography.  Each context
    advances its own current_frame counter independently.

    Rendering uses VK 1.3 dynamic rendering (vkCmdBeginRendering / vkCmdEndRendering).
    No VkRenderPass or VkFramebuffer objects are created or managed here.

==============================================================================================*/

/* Defined in vk_init.c, included after this file in the unity build. */
static vk_context_t* vk_ctx_get( i32 id );

/*==============================================================================================
    Frame begin / end
==============================================================================================*/

static rhi_command_list_t
vk_frame_begin( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return NULL;

    /* Handle deferred resize (never mid-recording; always between frames). */
    if ( ctx->resize_pending )
    {
        /* TODO: vk_swapchain_recreate( ctx ); */
        ctx->resize_pending = false;
    }

    /* Advance the global frame counter and select the staging slot. */
    g_vk.global_frame++;

    /* TODO (Vulkan implementation):

       u32 frame = ctx->current_frame;

       1. vkWaitForFences( g_vk.device, 1, &ctx->in_flight_fence[frame], VK_TRUE, UINT64_MAX )
          -- CPU blocks until this slot's GPU work is complete

       2. Flush the staging uploads queued last frame for this slot:
          vk_upload_flush( g_vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT )

       3. vkAcquireNextImageKHR( g_vk.device, ctx->swapchain, UINT64_MAX,
                                 ctx->image_available_sem[frame], VK_NULL_HANDLE,
                                 &ctx->image_index )
          -- VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR -> mark resize and return NULL

       4. vkResetFences( g_vk.device, 1, &ctx->in_flight_fence[frame] )

       5. vkResetCommandBuffer( ctx->command_buffers[frame], 0 )

       6. VkCommandBufferBeginInfo with VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
          vkBeginCommandBuffer( ctx->command_buffers[frame], &begin_info )

       7. Barrier: swapchain image UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL  (sync2 style)
          VkImageMemoryBarrier2 imb = {
              .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
              .srcAccessMask = 0,
              .dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .image         = ctx->swapchain_images[ctx->image_index],
          };
          vkCmdPipelineBarrier2( cmd_buf, &dep_info )

       8. Dynamic rendering begin:
          VkRenderingAttachmentInfo color_att = {
              .imageView   = ctx->swapchain_image_views[ctx->image_index],
              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
              .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
              .clearValue  = { .color = ctx->clear_color },
          };
          VkRenderingAttachmentInfo depth_att = {
              .imageView   = ctx->depth_view,
              .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
              .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
              .storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE,
              .clearValue  = { .depthStencil = { 1.0f, 0 } },
          };
          VkRenderingInfo ri = {
              .renderArea           = { {0,0}, ctx->swapchain_extent },
              .layerCount           = 1,
              .colorAttachmentCount = 1,
              .pColorAttachments    = &color_att,
              .pDepthAttachment     = &depth_att,
          };
          vkCmdBeginRendering( cmd_buf, &ri )

       Wire ctx->cmd_lists[frame] before returning:
          ctx->cmd_lists[frame].vk_cmd  = ctx->command_buffers[frame];
          ctx->cmd_lists[frame].ctx_id  = ctx_id;
          ctx->cmd_lists[frame].frame   = frame;
    */

    /* Placeholder: return a pointer to the cmd list struct (non-NULL; vk_cmd is VK_NULL_HANDLE
       until implementation is wired; callers guard on NULL but may pass through). */
    struct rhi_command_list_s* cmd = &ctx->cmd_lists[ ctx->current_frame ];
    cmd->ctx_id = ctx_id;
    cmd->frame  = ctx->current_frame;
    return cmd;
}

static void
vk_frame_end( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return;

    /* TODO (Vulkan implementation):

       u32 frame = ctx->current_frame;
       VkCommandBuffer cmd_buf = ctx->command_buffers[frame];

       1. vkCmdEndRendering( cmd_buf )

       2. Barrier: swapchain image COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR  (sync2)
          VkImageMemoryBarrier2 imb = {
              .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
              .dstAccessMask = 0,
              .oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              .image         = ctx->swapchain_images[ctx->image_index],
          };

       3. vkEndCommandBuffer( cmd_buf )

       4. vkQueueSubmit2 (sync2 path):
          VkSemaphoreSubmitInfo wait_sem = {
              .semaphore   = ctx->image_available_sem[frame],
              .stageMask   = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          };
          VkSemaphoreSubmitInfo signal_sem = {
              .semaphore = ctx->render_finished_sem[frame],
              .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          };
          VkCommandBufferSubmitInfo cmd_info = { .commandBuffer = cmd_buf };
          VkSubmitInfo2 sub = { .waitSemaphoreInfoCount   = 1, .pWaitSemaphoreInfos   = &wait_sem,
                                .commandBufferInfoCount   = 1, .pCommandBufferInfos   = &cmd_info,
                                .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal_sem };
          vkQueueSubmit2( g_vk.graphics_queue, 1, &sub, ctx->in_flight_fence[frame] )

       5. vkQueuePresentKHR:
          VkPresentInfoKHR pi = {
              .waitSemaphoreCount = 1,
              .pWaitSemaphores    = &ctx->render_finished_sem[frame],
              .swapchainCount     = 1,
              .pSwapchains        = &ctx->swapchain,
              .pImageIndices      = &ctx->image_index,
          };
          result = vkQueuePresentKHR( g_vk.present_queue, &pi )
          if result == VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR:
              ctx->resize_pending = true
    */

    ctx->current_frame = ( ctx->current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
}

/*==============================================================================================
    Commands  (v0 -- expanded as each subsystem comes online)
==============================================================================================*/

static void
vk_cmd_clear_color( rhi_command_list_t cmd, f32 r, f32 g, f32 b, f32 a )
{
    if ( !cmd )
        return;

    /* Stores the clear color for the context; consumed by vkCmdBeginRendering loadOp.
       This must be called before frame_begin returns to affect the current frame. */
    vk_context_t* ctx = vk_ctx_get( cmd->ctx_id );
    if ( !ctx )
        return;

    ctx->clear_color.float32[ 0 ] = r;
    ctx->clear_color.float32[ 1 ] = g;
    ctx->clear_color.float32[ 2 ] = b;
    ctx->clear_color.float32[ 3 ] = a;
}

static void
vk_cmd_set_viewport( rhi_command_list_t cmd, const rhi_viewport_t* vp )
{
    UNUSED( cmd );
    UNUSED( vp );
    /* TODO: VkViewport vkvp = { vp->x, vp->y, vp->width, vp->height, ... };
             vkCmdSetViewport( cmd->vk_cmd, 0, 1, &vkvp ) */
}

static void
vk_cmd_set_scissor( rhi_command_list_t cmd, const rhi_rect_t* rect )
{
    UNUSED( cmd );
    UNUSED( rect );
    /* TODO: VkRect2D sc = { {rect->x, rect->y}, {rect->width, rect->height} };
             vkCmdSetScissor( cmd->vk_cmd, 0, 1, &sc ) */
}

static void
vk_cmd_bind_pipeline( rhi_command_list_t cmd, rhi_pipeline_t pipeline )
{
    UNUSED( cmd );
    UNUSED( pipeline );
    /* TODO: VkPipeline vkp = vk_pipeline_get( pipeline );
             vkCmdBindPipeline( cmd->vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp ) */
}

static void
vk_cmd_bind_vertex_buffer( rhi_command_list_t cmd, rhi_buffer_t buf, u32 offset )
{
    UNUSED( cmd );
    UNUSED( buf );
    UNUSED( offset );
    /* TODO: VkDeviceSize off = offset;
             vkCmdBindVertexBuffers( cmd->vk_cmd, 0, 1, &g_vk.buffers[idx].buffer, &off ) */
}

static void
vk_cmd_bind_index_buffer( rhi_command_list_t cmd, rhi_buffer_t buf, u32 offset,
                           rhi_index_type_t type )
{
    UNUSED( cmd );
    UNUSED( buf );
    UNUSED( offset );
    UNUSED( type );
    /* TODO: VkIndexType vkt = (type == RHI_INDEX_TYPE_UINT16) ? VK_INDEX_TYPE_UINT16 : ...;
             vkCmdBindIndexBuffer( cmd->vk_cmd, ..., offset, vkt ) */
}

static void
vk_cmd_push_constants( rhi_command_list_t cmd, const void* data, u32 size, u32 offset )
{
    UNUSED( cmd );
    UNUSED( data );
    UNUSED( size );
    UNUSED( offset );
    /* TODO: vkCmdPushConstants( cmd->vk_cmd, g_vk.pipeline_layout,
                                 VK_SHADER_STAGE_ALL_GRAPHICS, offset, size, data ) */
}

static void
vk_cmd_draw( rhi_command_list_t cmd, const rhi_draw_args_t* args )
{
    UNUSED( cmd );
    UNUSED( args );
    /* TODO: vkCmdDraw( cmd->vk_cmd, args->vertex_count, args->instance_count,
                        args->first_vertex, args->first_instance ) */
}

static void
vk_cmd_draw_indexed( rhi_command_list_t cmd, const rhi_draw_indexed_args_t* args )
{
    UNUSED( cmd );
    UNUSED( args );
    /* TODO: vkCmdDrawIndexed( cmd->vk_cmd, args->index_count, args->instance_count,
                               args->first_index, args->vertex_offset, args->first_instance ) */
}

static void
vk_cmd_bind_bindless( rhi_command_list_t cmd )
{
    UNUSED( cmd );
    /* TODO: vkCmdBindDescriptorSets( cmd->vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      g_vk.pipeline_layout, 0, 1, &g_vk.bindless_set, 0, NULL ) */
}

/*============================================================================================*/
