/*==============================================================================================

    vulkan/vk_command.c — VkCommandPool + per-frame VkCommandBuffer allocation.

    One pool, VK_MAX_FRAMES_IN_FLIGHT command buffers. Each frame's buffer is
    reset (via VkCommandPool reset, or vkResetCommandBuffer per-buffer) at the
    top of frame_begin once the previous fence on that slot has been signaled.

==============================================================================================*/

static bool
vk_command_create( void )
{
    printf( "[rhi:vk] command_create (placeholder)\n" );

    /* TODO (Vulkan implementation):
       Pool:
       - VkCommandPoolCreateInfo with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         queueFamilyIndex = g_vk.graphics_queue_family
       - vkCreateCommandPool → g_vk.command_pool

       Buffers:
       - VkCommandBufferAllocateInfo with count = VK_MAX_FRAMES_IN_FLIGHT,
         level = PRIMARY
       - vkAllocateCommandBuffers → g_vk.command_buffers[] */

    return true;
}

static void
vk_command_destroy( void )
{
    printf( "[rhi:vk] command_destroy (placeholder)\n" );

    /* TODO (Vulkan implementation):
       - vkDeviceWaitIdle
       - vkDestroyCommandPool (frees buffers automatically) */
}

/*============================================================================================*/
