/*==============================================================================================

    vulkan/vk_frame.c — Per-frame orchestration.

    Implements rhi()->frame_begin and frame_end. These are the only RHI
    functions called every frame, so the per-frame Vulkan choreography lives
    here. cmd_* recording functions (currently just cmd_clear_color) also live
    here in v0; when there are more commands they'll move to dedicated files.

==============================================================================================*/

/*==============================================================================================
    Dummy non-NULL handle for the placeholder phase
==============================================================================================*/

/* When Vulkan is implemented this is replaced by a pointer to a real internal
   struct holding the VkCommandBuffer for the current frame. The dummy lets the
   renderer's code path exercise the full frame_begin → cmd → frame_end flow
   today without producing NULL handles that would force defensive checks
   everywhere. */

static char g_vk_dummy_cmd;

/*==============================================================================================
    Frame begin / end
==============================================================================================*/

static rhi_command_list_t
vk_frame_begin( void )
{
    if ( !g_vk.initialized )
        return NULL;

    if ( g_vk.resize_pending )
    {
        /* TODO: call vk_swapchain_recreate(), clear the flag. */
        g_vk.resize_pending = false;
    }

    /* TODO (Vulkan implementation):
       1. vkWaitForFences( g_vk.device, 1, &g_vk.in_flight_fence[g_vk.current_frame],
                           VK_TRUE, UINT64_MAX )
       2. vkAcquireNextImageKHR( g_vk.device, g_vk.swapchain, UINT64_MAX,
                                 g_vk.image_available_sem[g_vk.current_frame],
                                 VK_NULL_HANDLE, &image_index )
          - if VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR:
                g_vk.resize_pending = true; return NULL;
       3. vkResetFences( g_vk.device, 1, &g_vk.in_flight_fence[g_vk.current_frame] )
       4. vkResetCommandBuffer( g_vk.command_buffers[g_vk.current_frame], 0 )
       5. vkBeginCommandBuffer with ONE_TIME_SUBMIT_BIT
       6. Image layout transition: swapchain image UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
       7. vkCmdBeginRendering (dynamic rendering) with the swapchain image view */

    return (rhi_command_list_t)&g_vk_dummy_cmd;
}

static void
vk_frame_end( void )
{
    if ( !g_vk.initialized )
        return;

    /* TODO (Vulkan implementation):
       1. vkCmdEndRendering
       2. Image layout transition: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
       3. vkEndCommandBuffer
       4. vkQueueSubmit with:
            - waitSemaphore   = image_available_sem[current_frame]
            - waitStage       = COLOR_ATTACHMENT_OUTPUT
            - signalSemaphore = render_finished_sem[current_frame]
            - fence           = in_flight_fence[current_frame]
       5. vkQueuePresentKHR with render_finished_sem as wait
          - if VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR:
                g_vk.resize_pending = true
       6. g_vk.current_frame = ( g_vk.current_frame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT */
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
       This function should therefore stash the clear color into vk_state and
       vk_frame_begin's vkCmdBeginRendering reads it.

       Alternative: vkCmdClearColorImage outside a render pass (transitions
       are extra work) — generally avoid this path. */
}

/*============================================================================================*/
