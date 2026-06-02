/*==============================================================================================

    vulkan/vk_swapchain.c -- VkSurfaceKHR, VkSwapchainKHR, image views, and depth buffer.

    All functions operate on a single vk_context_t so multiple windows each own
    independent surface + swapchain + depth buffer simultaneously.

    Surface  -> requires: instance + native window
    Swapchain -> requires: device + surface
    Depth    -> requires: device + swapchain extent

==============================================================================================*/

/* Forward; called from vk_frame.c when resize_pending is set. */
static bool vk_swapchain_recreate( vk_context_t* ctx );
/* Forward; creates the per-context depth buffer matching the swapchain extent. */
static bool vk_depth_create( vk_context_t* ctx );
static void vk_depth_destroy( vk_context_t* ctx );

/*==============================================================================================
    Surface
==============================================================================================*/

static bool
vk_surface_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] surface_create ctx=%d win=%d (placeholder)\n", ctx->id, ctx->win_id );

#if OS_WINDOWS
    /* TODO:
       VkWin32SurfaceCreateInfoKHR ci = {
           .sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
           .hwnd      = (HWND)ctx->native_window,
           .hinstance = GetModuleHandle(NULL),
       };
       vkCreateWin32SurfaceKHR( g_vk.instance, &ci, g_vk.alloc_cb, &ctx->surface )
    */
#elif OS_LINUX && defined( VK_USE_PLATFORM_WAYLAND_KHR )
    /* TODO:
       VkWaylandSurfaceCreateInfoKHR ci = {
           .sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
           .display = wl_display,
           .surface = (struct wl_surface*)ctx->native_window,
       };
       vkCreateWaylandSurfaceKHR( g_vk.instance, &ci, g_vk.alloc_cb, &ctx->surface )
    */
#elif OS_LINUX
    /* TODO (Xlib):
       VkXlibSurfaceCreateInfoKHR ci = {
           .sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
           .dpy    = x11_display,
           .window = (Window)ctx->native_window,
       };
       vkCreateXlibSurfaceKHR( g_vk.instance, &ci, g_vk.alloc_cb, &ctx->surface )
    */
#elif OS_MAC
    /* TODO (MoltenVK):
       VkMetalSurfaceCreateInfoEXT ci = {
           .sType  = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
           .pLayer = (CAMetalLayer*)ctx->native_window,
       };
       vkCreateMetalSurfaceEXT( g_vk.instance, &ci, g_vk.alloc_cb, &ctx->surface )
    */
#endif

    return true;
}

static void
vk_surface_destroy( vk_context_t* ctx )
{
    printf( "[rhi:vk] surface_destroy ctx=%d (placeholder)\n", ctx->id );

    /* TODO: vkDestroySurfaceKHR( g_vk.instance, ctx->surface, g_vk.alloc_cb ) */
}

/*==============================================================================================
    Swapchain
==============================================================================================*/

static bool
vk_swapchain_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] swapchain_create ctx=%d (placeholder)\n", ctx->id );

    /* TODO (Vulkan implementation):

       Query surface capabilities:
           vkGetPhysicalDeviceSurfaceCapabilitiesKHR -> caps
           vkGetPhysicalDeviceSurfaceFormatsKHR      -> pick VK_FORMAT_B8G8R8A8_SRGB +
                                                        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
           vkGetPhysicalDeviceSurfacePresentModesKHR -> pick MAILBOX, fallback FIFO

       Extent:
           if caps.currentExtent == UINT32_MAX: clamp (ctx->width, ctx->height) to min/max
           else: use caps.currentExtent

       Image count:
           caps.minImageCount + 1, clamped to caps.maxImageCount (0 = no max)

       Sharing mode:
           if graphics_queue_family == present_queue_family: EXCLUSIVE
           else: CONCURRENT with both families listed

       VkSwapchainCreateInfoKHR + vkCreateSwapchainKHR -> ctx->swapchain
       vkGetSwapchainImagesKHR  -> ctx->swapchain_images[]
       For each image: vkCreateImageView -> ctx->swapchain_image_views[]
       Record ctx->swapchain_extent, ctx->surface_format, ctx->present_mode

       Then create the matched depth buffer:
           vk_depth_create( ctx )
    */

    return true;
}

static void
vk_swapchain_destroy( vk_context_t* ctx )
{
    printf( "[rhi:vk] swapchain_destroy ctx=%d (placeholder)\n", ctx->id );

    /* TODO:
       vk_depth_destroy( ctx )
       For each image view: vkDestroyImageView( g_vk.device, ctx->swapchain_image_views[i], ... )
       vkDestroySwapchainKHR( g_vk.device, ctx->swapchain, g_vk.alloc_cb )
       Zero the swapchain handles.
    */

    ( void )vk_swapchain_recreate;    /* suppress unused-function warning until wired */
}

static bool
vk_swapchain_recreate( vk_context_t* ctx )
{
    printf( "[rhi:vk] swapchain_recreate ctx=%d (placeholder)\n", ctx->id );

    /* TODO:
       Pass ctx->swapchain as oldSwapchain in VkSwapchainCreateInfoKHR for zero-gap handoff.
       vk_swapchain_destroy( ctx ) -- destroys views and depth; old VkSwapchainKHR freed here
       vk_swapchain_create( ctx )  -- creates new swapchain + depth at new size
       Must only be called between frames (never mid-recording).
    */

    return true;
}

/*==============================================================================================
    Depth buffer  (owned per context; recreated on resize along with the swapchain)
==============================================================================================*/

static bool
vk_depth_create( vk_context_t* ctx )
{
    printf( "[rhi:vk] depth_create ctx=%d (placeholder)\n", ctx->id );

    /* TODO:
       Format selection: probe VK_FORMAT_D32_SFLOAT, fallback VK_FORMAT_D24_UNORM_S8_UINT,
       last resort VK_FORMAT_D16_UNORM.  Check vkGetPhysicalDeviceFormatProperties for
       VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT.

       VkImageCreateInfo:
           imageType   = VK_IMAGE_TYPE_2D
           format      = ctx->depth_format
           extent      = { ctx->swapchain_extent.width, ctx->swapchain_extent.height, 1 }
           mipLevels   = 1
           arrayLayers = 1
           samples     = VK_SAMPLE_COUNT_1_BIT
           tiling      = VK_IMAGE_TILING_OPTIMAL
           usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
       vkCreateImage -> ctx->depth_image
       vkAllocateMemory (device-local) + vkBindImageMemory -> ctx->depth_memory

       VkImageViewCreateInfo with VK_IMAGE_ASPECT_DEPTH_BIT -> ctx->depth_view
    */

    return true;
}

static void
vk_depth_destroy( vk_context_t* ctx )
{
    /* TODO:
       vkDestroyImageView( g_vk.device, ctx->depth_view,   g_vk.alloc_cb )
       vkDestroyImage    ( g_vk.device, ctx->depth_image,  g_vk.alloc_cb )
       vkFreeMemory      ( g_vk.device, ctx->depth_memory, g_vk.alloc_cb )
    */
    UNUSED( ctx );
}

/*============================================================================================*/
