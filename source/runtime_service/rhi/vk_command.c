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
    /* Per-buffer reset: lets frame_begin call vkResetCommandBuffer individually
       without needing to reset the whole pool. */
    VkCommandPoolCreateInfo pool_ci = { 0 };
    pool_ci.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex        = vk.graphics_queue_family;

    VkResult r = vkCreateCommandPool( vk.device, &pool_ci, vk.alloc_cb, &ctx->command_pool );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "command_create: vkCreateCommandPool: %s", string_VkResult( r ) );
        return false;
    }

    /* Allocate one primary buffer per frame-in-flight slot. */
    VkCommandBufferAllocateInfo alloc_info = { 0 };
    alloc_info.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool                 = ctx->command_pool;
    alloc_info.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount          = VK_MAX_FRAMES_IN_FLIGHT;

    r = vkAllocateCommandBuffers( vk.device, &alloc_info, ctx->command_buffers );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "command_create: vkAllocateCommandBuffers: %s", string_VkResult( r ) );
        vkDestroyCommandPool( vk.device, ctx->command_pool, vk.alloc_cb );
        ctx->command_pool = VK_NULL_HANDLE;
        return false;
    }

    /* Pre-populate cmd_list identity fields; vk_cmd is overwritten each frame_begin. */
    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        ctx->cmd_lists[ i ].vk_cmd = ctx->command_buffers[ i ];
        ctx->cmd_lists[ i ].ctx_id = ctx->id;
        ctx->cmd_lists[ i ].frame  = i;
    }

    /* GPU frame timer: two timestamp queries per frame slot (see vk_context_t).  Skipped when
       the device cannot timestamp graphics queues; frame_begin/frame_end test the handle. */
    ctx->query_pool = VK_NULL_HANDLE;
    ctx->gpu_ms     = 0.0f;
    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
        ctx->gpu_ts_written[ i ] = false;

    if ( vk.physical_device_props.limits.timestampComputeAndGraphics )
    {
        VkQueryPoolCreateInfo qp_ci = { 0 };
        qp_ci.sType                 = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qp_ci.queryType             = VK_QUERY_TYPE_TIMESTAMP;
        qp_ci.queryCount            = 2u * VK_MAX_FRAMES_IN_FLIGHT;

        r = vkCreateQueryPool( vk.device, &qp_ci, vk.alloc_cb, &ctx->query_pool );
        if ( r != VK_SUCCESS )
        {
            /* The timer is diagnostics, not correctness: render without it. */
            LOG_WARN( "command_create: vkCreateQueryPool: %s (gpu timer disabled)",
                      string_VkResult( r ) );
            ctx->query_pool = VK_NULL_HANDLE;
        }
    }

    LOG_INFO( "command_create: OK (ctx %d)", ctx->id );
    return true;
}

static void
vk_command_destroy( vk_context_t* ctx )
{
    if ( ctx->command_pool == VK_NULL_HANDLE )
        return;

    /* Destroying the pool implicitly frees all command buffers allocated from it. */
    vkDestroyCommandPool( vk.device, ctx->command_pool, vk.alloc_cb );
    ctx->command_pool = VK_NULL_HANDLE;

    if ( ctx->query_pool != VK_NULL_HANDLE )
    {
        vkDestroyQueryPool( vk.device, ctx->query_pool, vk.alloc_cb );
        ctx->query_pool = VK_NULL_HANDLE;
    }

    for ( u32 i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; ++i )
    {
        ctx->command_buffers[ i ]  = VK_NULL_HANDLE;
        ctx->cmd_lists[ i ].vk_cmd = VK_NULL_HANDLE;
    }
}

/*============================================================================================*/
