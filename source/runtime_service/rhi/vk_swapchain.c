/*==============================================================================================

    vulkan/vk_swapchain.c — VkSurfaceKHR + VkSwapchainKHR + image views.

    Surface creation happens after vk_instance_create() (needs instance + window).
    Swapchain creation happens after vk_device_create() (needs device + surface).

    Lifetime: created in init, destroyed in shutdown, recreated on resize.

==============================================================================================*/

static bool
vk_surface_create( void )
{
    printf( "[rhi:vk] surface_create (placeholder)\n" );

    /* TODO (Vulkan implementation, Windows):
       - VkWin32SurfaceCreateInfoKHR with HINSTANCE and HWND
            HWND      = (HWND)g_vk.native_window
            HINSTANCE = GetModuleHandle(NULL)
       - vkCreateWin32SurfaceKHR → g_vk.surface */

    return true;
}

static void
vk_surface_destroy( void )
{
    printf( "[rhi:vk] surface_destroy (placeholder)\n" );

    /* TODO: vkDestroySurfaceKHR( g_vk.instance, g_vk.surface, NULL ) */
}

static bool
vk_swapchain_create( void )
{
    printf( "[rhi:vk] swapchain_create (placeholder)\n" );

    /* TODO (Vulkan implementation):
       Query support:
       - vkGetPhysicalDeviceSurfaceCapabilitiesKHR
       - vkGetPhysicalDeviceSurfaceFormatsKHR  → pick BGRA8 SRGB if available
       - vkGetPhysicalDeviceSurfacePresentModesKHR → pick MAILBOX if available,
         fall back to FIFO

       Choose extent:
       - If currentExtent is (UINT32_MAX, UINT32_MAX), use g_vk.width/height
         clamped to capabilities.minImageExtent..maxImageExtent

       Choose image count:
       - capabilities.minImageCount + 1, clamped to maxImageCount

       Create:
       - VkSwapchainCreateInfoKHR (graphics + present family sharing if different)
       - vkCreateSwapchainKHR → g_vk.swapchain
       - vkGetSwapchainImagesKHR → g_vk.swapchain_images[]
       - For each image: vkCreateImageView → g_vk.swapchain_image_views[]
       - Record g_vk.swapchain_extent */

    return true;
}

static void
vk_swapchain_destroy( void )
{
    printf( "[rhi:vk] swapchain_destroy (placeholder)\n" );

    /* TODO (Vulkan implementation):
       - vkDeviceWaitIdle
       - For each image view: vkDestroyImageView
       - vkDestroySwapchainKHR */
}

static bool
vk_swapchain_recreate( void )
{
    printf( "[rhi:vk] swapchain_recreate (placeholder)\n" );

    /* TODO: vk_swapchain_destroy() then vk_swapchain_create().
       Or use VkSwapchainCreateInfoKHR::oldSwapchain to hand off the old one
       cleanly. Must be called between frames, never during recording. */

    return true;
}

/*============================================================================================*/
