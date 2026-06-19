/*==============================================================================================

    vulkan/vk_sync.c -- Per-frame synchronization objects, owned per context.

    For VK_MAX_FRAMES_IN_FLIGHT (=2) frames per context:
      - image_available_sem  : signaled when vkAcquireNextImageKHR completes
      - render_finished_sem  : signaled when graphics queue submit completes;
                               waited on by vkQueuePresentKHR
      - in_flight_fence      : signaled when the frame's GPU work is done;
                               CPU waits on it before reusing the slot

==============================================================================================*/

static void vk_sync_destroy( vk_context_t* ctx );

static bool
vk_sync_create( vk_context_t* ctx )
{
    VkSemaphoreCreateInfo sem_ci = { 0 };
    sem_ci.sType                 = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    /* Pre-signal fences so the first vkWaitForFences on each slot returns immediately. */
    VkFenceCreateInfo fence_ci   = { 0 };
    fence_ci.sType               = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags               = VK_FENCE_CREATE_SIGNALED_BIT;

    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        VkResult r;
        r = vkCreateSemaphore( vk.device, &sem_ci, vk.alloc_cb, &ctx->image_available_sem[ i ] );
        if ( r != VK_SUCCESS ) {
             LOG_ERROR( "sync_create: image_available_sem[%u]: %s", i, string_VkResult( r ) );
             goto fail;
        }
        r = vkCreateFence( vk.device, &fence_ci, vk.alloc_cb, &ctx->in_flight_fence[ i ] );
        if ( r != VK_SUCCESS ) {
             LOG_ERROR( "sync_create: in_flight_fence[%u]: %s", i, string_VkResult( r ) );
             goto fail;
        }
    }

    /* render_finished_sem is indexed by swapchain image, not frame slot.
       Safe to reuse sem[i] only after vkAcquireNextImageKHR returns image i again,
       which guarantees the previous vkQueuePresentKHR consumed it. */
    for ( u32 i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; ++i )
    {
        VkResult r = vkCreateSemaphore( vk.device, &sem_ci, vk.alloc_cb, &ctx->render_finished_sem[ i ] );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "sync_create: render_finished_sem[%u]: %s", i, string_VkResult( r ) );
            goto fail;
        }
    }

    LOG_TRACE( "sync_create: OK (ctx %d, %u frame slots, %u image slots)",
              ctx->id, VK_MAX_FRAMES_IN_FLIGHT, VK_MAX_SWAPCHAIN_IMAGES );
    return true;

fail:
    vk_sync_destroy( ctx );
    return false;
}

static void
vk_sync_destroy( vk_context_t* ctx )
{
    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        if ( ctx->in_flight_fence[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyFence( vk.device, ctx->in_flight_fence[ i ], vk.alloc_cb );
            ctx->in_flight_fence[ i ] = VK_NULL_HANDLE;
        }
        if ( ctx->image_available_sem[ i ] != VK_NULL_HANDLE )
        {
            vkDestroySemaphore( vk.device, ctx->image_available_sem[ i ], vk.alloc_cb );
            ctx->image_available_sem[ i ] = VK_NULL_HANDLE;
        }
    }
    for ( u32 i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; ++i )
    {
        if ( ctx->render_finished_sem[ i ] != VK_NULL_HANDLE )
        {
            vkDestroySemaphore( vk.device, ctx->render_finished_sem[ i ], vk.alloc_cb );
            ctx->render_finished_sem[ i ] = VK_NULL_HANDLE;
        }
    }
}

/*============================================================================================*/
