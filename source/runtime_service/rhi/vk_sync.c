/*==============================================================================================

    vulkan/vk_sync.c — Per-frame synchronization objects.

    For VK_MAX_FRAMES_IN_FLIGHT (=2) frames:
      - image_available_sem  : signaled when vkAcquireNextImageKHR completes
      - render_finished_sem  : signaled when graphics queue submit completes;
                               waited on by vkQueuePresentKHR
      - in_flight_fence      : signaled when the frame's GPU work is done;
                               CPU waits on it before reusing the slot

==============================================================================================*/

static bool
vk_sync_create( void )
{
    printf( "[rhi:vk] sync_create (placeholder)\n" );

    /* TODO (Vulkan implementation):
       For i in [0..VK_MAX_FRAMES_IN_FLIGHT):
         - vkCreateSemaphore → g_vk.image_available_sem[i]
         - vkCreateSemaphore → g_vk.render_finished_sem[i]
         - vkCreateFence (with VK_FENCE_CREATE_SIGNALED_BIT so the first frame
           doesn't block) → g_vk.in_flight_fence[i] */

    return true;
}

static void
vk_sync_destroy( void )
{
    printf( "[rhi:vk] sync_destroy (placeholder)\n" );

    /* TODO (Vulkan implementation):
       - vkDeviceWaitIdle first
       - For each i: vkDestroySemaphore × 2, vkDestroyFence */
}

/*============================================================================================*/
