/*==============================================================================================

    vulkan/vk_swapchain.c -- VkSurfaceKHR + VkSwapchainKHR + image views.

    All functions operate on a single vk_context_t, so multiple windows can
    each own their own surface and swapchain simultaneously.

    Surface creation happens after vk_instance_create() (needs instance + window).
    Swapchain creation happens after vk_device_create() (needs device + surface).

==============================================================================================*/

static bool vk_swapchain_recreate( vk_context_t* ctx );   /* forward */

static bool
vk_surface_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] surface_create ctx=%d win=%d (placeholder)\n", ctx->id, ctx->win_id );

    /* TODO (Vulkan implementation, Windows):
       - VkWin32SurfaceCreateInfoKHR:
            HWND      = (HWND)ctx->native_window
            HINSTANCE = GetModuleHandle(NULL)
       - vkCreateWin32SurfaceKHR( g_vk.instance, &info, NULL, &ctx->surface ) */

    return true;
}

static void
vk_surface_destroy( vk_context_t* ctx )
{
    printf( "[rhi:vk] surface_destroy ctx=%d (placeholder)\n", ctx->id );

    /* TODO: vkDestroySurfaceKHR( g_vk.instance, ctx->surface, NULL ) */
}

static bool
vk_swapchain_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] swapchain_create ctx=%d (placeholder)\n", ctx->id );

    /* TODO (Vulkan implementation):
       Query support:
       - vkGetPhysicalDeviceSurfaceCapabilitiesKHR( g_vk.physical_device, ctx->surface, ... )
       - vkGetPhysicalDeviceSurfaceFormatsKHR      -> pick BGRA8 SRGB if available
       - vkGetPhysicalDeviceSurfacePresentModesKHR -> pick MAILBOX, fall back to FIFO

       Choose extent:
       - If currentExtent is (UINT32_MAX, UINT32_MAX), use ctx->width/height
         clamped to capabilities.minImageExtent..maxImageExtent

       Choose image count:
       - capabilities.minImageCount + 1, clamped to maxImageCount

       Create:
       - VkSwapchainCreateInfoKHR (handle graphics + present family sharing)
       - vkCreateSwapchainKHR -> ctx->swapchain
       - vkGetSwapchainImagesKHR -> ctx->swapchain_images[]
       - For each image: vkCreateImageView -> ctx->swapchain_image_views[]
       - Record ctx->swapchain_extent */

    return true;
}

static void
vk_swapchain_destroy( vk_context_t* ctx )
{
    printf( "[rhi:vk] swapchain_destroy ctx=%d (placeholder)\n", ctx->id );

    /* TODO:
       - vkDeviceWaitIdle
       - For each image view: vkDestroyImageView
       - vkDestroySwapchainKHR */

    ( void )vk_swapchain_recreate;
}

static bool
vk_swapchain_recreate( vk_context_t* ctx )
{
    printf( "[rhi:vk] swapchain_recreate ctx=%d (placeholder)\n", ctx->id );

    /* TODO: vk_swapchain_destroy(ctx) then vk_swapchain_create(ctx).
       Or pass oldSwapchain in VkSwapchainCreateInfoKHR for a clean handoff.
       Must be called between frames, never mid-recording. */

    return true;
}

/*============================================================================================*/
