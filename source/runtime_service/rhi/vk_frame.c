/*==============================================================================================

    vulkan/vk_frame.c -- Per-frame orchestration.

    frame_begin and frame_end take a ctx_id, resolve it to the vk_context_t, and
    drive the per-window Vulkan choreography. Each window advances its own
    current_frame counter independently.

==============================================================================================*/

/* Defined in vk_init.c, included after this file in the unity build. */
static vk_context_t* vk_ctx_get( i32 id );

/* Dummy non-NULL handle for the placeholder phase.
   Replaced by a pointer into the real per-frame state once Vulkan is wired. */
static char g_vk_dummy_cmd;

/*==============================================================================================
    Frame begin / end
==============================================================================================*/

static rhi_command_list_t
vk_frame_begin( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return NULL;

    if ( ctx->resize_pending )
    {
        /* TODO: vk_swapchain_recreate( ctx ); */
        ctx->resize_pending = false;
    }

    /* TODO (Vulkan implementation):
       1. vkWaitForFences( g_vk.device, 1,
                           &ctx->in_flight_fence[ctx->current_frame], VK_TRUE, UINT64_MAX )
       2. vkAcquireNextImageKHR( g_vk.device, ctx->swapchain, UINT64_MAX,
                                 ctx->image_available_sem[ctx->current_frame],
                                 VK_NULL_HANDLE, &image_index )
          - if VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR:
                ctx->resize_pending = true; return NULL;
       3. vkResetFences( g_vk.device, 1, &ctx->in_flight_fence[ctx->current_frame] )
       4. vkResetCommandBuffer( ctx->command_buffers[ctx->current_frame], 0 )
       5. vkBeginCommandBuffer with ONE_TIME_SUBMIT_BIT
       6. Image layout transition: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
       7. vkCmdBeginRendering (dynamic rendering) with the swapchain image view */

    return ( rhi_command_list_t )&g_vk_dummy_cmd;
}

static void
vk_frame_end( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return;

    /* TODO (Vulkan implementation):
       1. vkCmdEndRendering
       2. Image layout transition: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR
       3. vkEndCommandBuffer
       4. vkQueueSubmit:
            waitSemaphore   = ctx->image_available_sem[ctx->current_frame]
            waitStage       = COLOR_ATTACHMENT_OUTPUT
            signalSemaphore = ctx->render_finished_sem[ctx->current_frame]
            fence           = ctx->in_flight_fence[ctx->current_frame]
       5. vkQueuePresentKHR with render_finished_sem as wait
          - if VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR:
                ctx->resize_pending = true */

    ctx->current_frame = ( ctx->current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
}

/*==============================================================================================
    Commands (v0)
==============================================================================================*/

static void
vk_cmd_clear_color( rhi_command_list_t cmd, f32 r, f32 g, f32 b, f32 a )
{
    UNUSED( cmd );
    UNUSED( r ); UNUSED( g ); UNUSED( b ); UNUSED( a );

    /* TODO (Vulkan implementation):
       With dynamic rendering, the clear is part of VkRenderingAttachmentInfo
       in vkCmdBeginRendering (loadOp = CLEAR, clearValue = { r, g, b, a }).
       Stash the color into vk_context_t and read it in vkCmdBeginRendering. */
}

/*============================================================================================*/
