/*==============================================================================================

    vulkan/vk_command.c -- VkCommandPool + per-frame VkCommandBuffer allocation,
    owned per context.

    One pool and VK_MAX_FRAMES_IN_FLIGHT command buffers per context. Each
    frame's buffer is reset at the top of frame_begin once the previous fence
    on that slot has been signaled.

==============================================================================================*/

static bool
vk_command_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] command_create ctx=%d (placeholder)\n", ctx->id );

    /* TODO (Vulkan implementation):
       Pool:
       - VkCommandPoolCreateInfo with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         queueFamilyIndex = vk.graphics_queue_family
       - vkCreateCommandPool -> ctx->command_pool

       Buffers:
       - VkCommandBufferAllocateInfo with count = VK_MAX_FRAMES_IN_FLIGHT,
         level = PRIMARY
       - vkAllocateCommandBuffers -> ctx->command_buffers[] */

    return true;
}

static void
vk_command_destroy( vk_context_t* ctx )
{
    printf( "[rhi:vk] command_destroy ctx=%d (placeholder)\n", ctx->id );

    /* TODO:
       - vkDeviceWaitIdle
       - vkDestroyCommandPool (frees command buffers automatically) */
}

/*============================================================================================*/
