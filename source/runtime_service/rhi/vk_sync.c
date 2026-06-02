/*==============================================================================================

    vulkan/vk_sync.c -- Per-frame synchronization objects, owned per context.

    For VK_MAX_FRAMES_IN_FLIGHT (=2) frames per context:
      - image_available_sem  : signaled when vkAcquireNextImageKHR completes
      - render_finished_sem  : signaled when graphics queue submit completes;
                               waited on by vkQueuePresentKHR
      - in_flight_fence      : signaled when the frame's GPU work is done;
                               CPU waits on it before reusing the slot

==============================================================================================*/

static bool
vk_sync_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] sync_create ctx=%d (placeholder)\n", ctx->id );

    /* TODO (Vulkan implementation):
       For i in [0..VK_MAX_FRAMES_IN_FLIGHT):
         - vkCreateSemaphore -> ctx->image_available_sem[i]
         - vkCreateSemaphore -> ctx->render_finished_sem[i]
         - vkCreateFence (VK_FENCE_CREATE_SIGNALED_BIT so first frame doesn't block)
                         -> ctx->in_flight_fence[i] */

    return true;
}

static void
vk_sync_destroy( vk_context_t* ctx )
{
    printf( "[rhi:vk] sync_destroy ctx=%d (placeholder)\n", ctx->id );

    /* TODO:
       - vkDeviceWaitIdle first
       - For each i: vkDestroySemaphore x2, vkDestroyFence */
}

/*============================================================================================*/
