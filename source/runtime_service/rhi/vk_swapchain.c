/*==============================================================================================

    vulkan/vk_swapchain.c -- VkSurfaceKHR, VkSwapchainKHR, image views, and depth buffer.

    All functions operate on a single vk_context_t so multiple windows each own
    independent surface + swapchain + depth buffer simultaneously.

    Surface   -> requires: instance + native window
    Swapchain -> requires: device + surface
    Depth     -> requires: device + swapchain extent

==============================================================================================*/
// clang-format off

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
    VkResult r;

#if OS_WINDOWS

    VkWin32SurfaceCreateInfoKHR ci = { 0 };
    ci.sType        = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hwnd         = (HWND)ctx->native_window;
    ci.hinstance    = GetModuleHandle( NULL );

    r = vkCreateWin32SurfaceKHR( vk.instance, &ci, vk.alloc_cb, &ctx->surface );

#elif OS_LINUX && defined( VK_USE_PLATFORM_WAYLAND_KHR )

    /* native_window is a wl_surface*; display must be passed via platform context */
    VkWaylandSurfaceCreateInfoKHR ci = { 0 };
    ci.sType        = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    ci.display      = NULL;    /* TODO: obtain wl_display from sys layer */
    ci.surface      = (struct wl_surface*)ctx->native_window;

    r = vkCreateWaylandSurfaceKHR( vk.instance, &ci, vk.alloc_cb, &ctx->surface );

#elif OS_LINUX

    /* native_window is a Window (XID); display must be passed via platform context */
    VkXlibSurfaceCreateInfoKHR ci = { 0 };
    ci.sType        = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    ci.dpy          = NULL;    /* TODO: obtain Display* from sys layer */
    ci.window       = (Window)(uintptr_t)ctx->native_window;

    r = vkCreateXlibSurfaceKHR( vk.instance, &ci, vk.alloc_cb, &ctx->surface );

#elif OS_MAC

    /* native_window is a CAMetalLayer* passed from the platform windowing layer */
    VkMetalSurfaceCreateInfoEXT ci = { 0 };
    ci.sType                       = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    ci.pLayer                      = (const CAMetalLayer*)ctx->native_window;

    r = vkCreateMetalSurfaceEXT( vk.instance, &ci, vk.alloc_cb, &ctx->surface );

#endif

    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "surface_create: %s (ctx %d)", string_VkResult( r ), ctx->id );
         return false;
    }

    /* Confirm the selected present queue family actually supports this surface. */
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR( vk.physical_device, vk.present_queue_family,
                                          ctx->surface, &supported );
    if ( !supported )
    {
        LOG_ERROR( "surface_create: present queue family %u does not support this surface (ctx %d)",
                   vk.present_queue_family, ctx->id );
        vkDestroySurfaceKHR( vk.instance, ctx->surface, vk.alloc_cb );
        ctx->surface = VK_NULL_HANDLE;
        return false;
    }

    LOG_INFO( "vk_surface_create: OK (ctx %d)", ctx->id );
    return true;
}

static void
vk_surface_destroy( vk_context_t* ctx )
{
    if ( ctx->surface == VK_NULL_HANDLE )
        return;

    vkDestroySurfaceKHR( vk.instance, ctx->surface, vk.alloc_cb );
    ctx->surface = VK_NULL_HANDLE;
}

/*==============================================================================================
    Swapchain Support -- Format selection helper
==============================================================================================*/

static VkSurfaceFormatKHR
vk_swapchain_pick_format( VkSurfaceFormatKHR* formats, u32 count )
{
    /* Prefer BGRA8_SRGB: most common on Win32/DXGI path and correct gamma. */
    for ( u32 i = 0; i < count; ++i )
    {
        if ( formats[ i ].format     == VK_FORMAT_B8G8R8A8_SRGB &&
             formats[ i ].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR )
            return formats[ i ];
    }
    /* Also accept RGBA8_SRGB for platforms that prefer it. */
    for ( u32 i = 0; i < count; ++i )
    {
        if ( formats[ i ].format     == VK_FORMAT_R8G8B8A8_SRGB &&
             formats[ i ].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR )
            return formats[ i ];
    }
    return formats[ 0 ];
}

/*==============================================================================================
    Swapchain Support -- Present-mode selection helper 
    
    For reference, do not modify.

    VK_PRESENT_MODE_IMMEDIATE_KHR (VSync OFF)
    * The engine submits a frame, and the display controller shows it immediately. 
      It does not wait for the monitor to finish its current refresh cycle.
    * Pros: Absolute lowest possible input latency.
    * Cons: Resulting "tearing" (horizontal lines where the old frame meets the new one). 
      Tearing is more visible at lower framerates and higher resolutions.

    VK_PRESENT_MODE_MAILBOX_KHR (Fast VSync / Triple Buffering)
    * This is a "non-blocking" VSync. The GPU keeps rendering frames into a 3-slot queue. 
      If the queue is full, the newest frame replaces the one waiting to be shown.
    * Pros: No tearing. Significantly lower latency than standard VSync because the
      monitor always grabs the most recent completed frame at the moment of refresh.
    * Cons: Uses more VRAM (3+ images). Requires the GPU to work harder (rendering
      frames that might never be seen) which can increase power consumption/heat.

    VK_PRESENT_MODE_FIFO_KHR (Standard VSync)
    * A traditional queue. If the queue is full, the engine "blocks" (stalls) until a 
      vertical blanking interval (VBlank) occurs and a slot opens up.
    * Pros: No tearing. Guaranteed to be supported by every Vulkan implementation.
      Perfect frame pacing if you hit the refresh rate exactly.
    * Cons: Highest input latency. If you miss the refresh rate (e.g., 59 FPS on a
      60Hz screen), the framerate effectively drops to 30 FPS (stutter).

    VK_PRESENT_MODE_FIFO_RELAXED_KHR (Late-Swap VSync)
    * How it works: If you are faster than the refresh rate, it behaves like FIFO (no
      tearing). If you are late (a frame takes too long), it behaves like IMMEDIATE
      to avoid the "drop to 30 FPS" stutter.
    * Pros: Good middle ground for systems that occasionally dip below target FPS.
    * Cons: Tearing only happens when you're lagging, which is exactly when you don't
      want the visual distraction.

    VK_PRESENT_MODE_FIFO_LATEST_READY_KHR
    * It’s a hybrid of FIFO and MAILBOX. Like FIFO, it waits for the
      VBlank (VSync). However, if your game rendered 3 frames since the last VBlank,
      it will discard the first 2 and show the most recent one.
    * Trade-offs: It gives you the "snappiness" of Mailbox but uses the strict queue
      logic of FIFO. It’s excellent for reducing the "VSync lag" on modern drivers
      that support it.
    * Verdict: If supported, this is usually superior to standard FIFO.
     
    The "New Tech" Impact (GSync / FreeSync / VRR)

    Variable Refresh Rate (VRR) technologies completely change the logic of
    vk_swapchain_pick_present_mode.

    With GSync/FreeSync Enabled:
    * Use FIFO: Counter-intuitively, FIFO is often the best choice for VRR. Within
      the monitor's range (e.g., 40Hz–144Hz), the monitor waits for the GPU. FIFO
      behaves like IMMEDIATE but without the tearing.
    * Frame Limiting: To stay in the VRR range, you should limit your game's FPS to
      ~3 FPS below the max refresh (e.g., 141 FPS for a 144Hz screen). If you exceed
      the max refresh, FIFO will revert to high-latency VSync behavior.

        The "Modern Ultra-Low Latency" Setup:
        1. GSync ON (Global Driver Setting).
        2. VSync ON (In-game or Driver).
        3. Frame Limiter (In-game) set to (RefreshRate - 3).
  
    This combination provides the lowest latency with zero tearing.


==============================================================================================*/

static VkPresentModeKHR
vk_swapchain_pick_present_mode( VkPresentModeKHR* modes, u32 count )
{
    if ( vk.has_vrr && vk.use_vrr_if_available )
    {
        /* VRR (GSync/FreeSync): monitor adapts its refresh to the GPU framerate.
           FIFO becomes tear-free adaptive sync; MAILBOX only wastes GPU cycles here. */

        /* 1. FIFO_LATEST_READY: adaptive sync + latest-frame = best VRR quality. */
        if ( vk.has_fifo_latest_ready )
        {
            for ( u32 i = 0; i < count; ++i ) {
                if ( modes[ i ] == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR )
                    return modes[ i ];
            }
        }
        /* 2. IMMEDIATE (vsync-off only): raw minimum latency as explicit user intent. */
        if ( vk.use_vsync == false )
        {
            for ( u32 i = 0; i < count; ++i ) {
                if ( modes[ i ] == VK_PRESENT_MODE_IMMEDIATE_KHR )
                    return modes[ i ];
            }
        }
        /* 2/3. FIFO: VRR turns this into tear-free adaptive presentation. */
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    if ( vk.use_vsync == true )
    {
        /* No VRR, vsync on: strict vblank-synced tear-free presentation. */

        /* 1. FIFO_LATEST_READY: reduces VSync lag on modern drivers. */
        if ( vk.has_fifo_latest_ready )
        {
            for ( u32 i = 0; i < count; ++i ) {
                if ( modes[ i ] == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR )
                    return modes[ i ];
            }
        }
        /* 2. FIFO_RELAXED: avoids the "miss one frame -> halve FPS" stutter on frame drops. */
        for ( u32 i = 0; i < count; ++i ) {
            if ( modes[ i ] == VK_PRESENT_MODE_FIFO_RELAXED_KHR )
                return modes[ i ];
        }
        /* 3. FIFO: mandatory per spec; classic vsync, no tearing. */
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    /* No VRR, vsync off: uncapped render, favor lowest latency. */

    /* 1. MAILBOX: uncapped render, no tearing, low latency. Best default for vsync-off. */
    for ( u32 i = 0; i < count; ++i ) {
        if ( modes[ i ] == VK_PRESENT_MODE_MAILBOX_KHR )
            return modes[ i ];
    }
    /* 2. IMMEDIATE: true minimum latency, may tear. */
    for ( u32 i = 0; i < count; ++i ) {
        if ( modes[ i ] == VK_PRESENT_MODE_IMMEDIATE_KHR )
            return modes[ i ];
    }
    /* 3. FIFO: guaranteed fallback. */
    return VK_PRESENT_MODE_FIFO_KHR;
}

/*==============================================================================================
    Swapchain
==============================================================================================*/

static bool
vk_swapchain_create( vk_context_t* ctx, VkSwapchainKHR old_swapchain )
{
    /* --- Query surface capabilities --- */

    VkSurfaceCapabilitiesKHR caps = { 0 };
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physical_device, ctx->surface, &caps );
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "vkGetPhysicalDeviceSurfaceCapabilitiesKHR: %s", string_VkResult( r ) );
         return false;
    }

    /* --- Pick surface format --- */

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physical_device, ctx->surface, &format_count, NULL );
    if ( format_count == 0 ) {
         LOG_ERROR( "no surface formats available (ctx %d)", ctx->id );
         return false;
    }

    VkSurfaceFormatKHR formats[ 32 ] = { 0 };
    if ( format_count > 32 ) format_count = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physical_device, ctx->surface, &format_count, formats );
    ctx->surface_format = vk_swapchain_pick_format( formats, format_count );

    /* --- Pick present mode --- */

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR( vk.physical_device, ctx->surface, &mode_count, NULL );

    VkPresentModeKHR modes[ 8 ] = { 0 };
    if ( mode_count > 8 ) mode_count = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR( vk.physical_device, ctx->surface, &mode_count, modes );
    ctx->present_mode = vk_swapchain_pick_present_mode( modes, mode_count );

    /* --- Determine swapchain extent --- */

    VkExtent2D extent;
    if ( caps.currentExtent.width != UINT32_MAX )
    {
        /* Surface dictates the exact extent -- 
           window system decides size (most common Win32 behavior). */
        extent = caps.currentExtent;
    }
    else
    {
        /* Flexible size, the window systems will adapt to our swapchain. */
        /* Clamp the requested dimensions to the surface-supported range. */

        u32 w = (u32)ctx->width;
        u32 h = (u32)ctx->height;
        if ( w < caps.minImageExtent.width  ) w = caps.minImageExtent.width;
        if ( w > caps.maxImageExtent.width  ) w = caps.maxImageExtent.width;
        if ( h < caps.minImageExtent.height ) h = caps.minImageExtent.height;
        if ( h > caps.maxImageExtent.height ) h = caps.maxImageExtent.height;
        extent.width  = w;
        extent.height = h;
    }
    ctx->swapchain_extent = extent;

    /* --- Determine image count based on present mode ---
       Mailbox needs 3 (on-screen + mailbox slot + rendering); others need only the minimum. */

    u32  image_count;
    if ( ctx->present_mode == VK_PRESENT_MODE_MAILBOX_KHR )
         image_count = ( caps.minImageCount < 3 ) ? 3 : caps.minImageCount;
    else
         image_count = ( caps.minImageCount );

    if ( caps.maxImageCount > 0 && image_count > caps.maxImageCount )
         image_count = caps.maxImageCount;
    if ( image_count > VK_MAX_SWAPCHAIN_IMAGES )
         image_count = VK_MAX_SWAPCHAIN_IMAGES;

    /* --- Build swapchain create info --- */

    VkSwapchainCreateInfoKHR ci = { 0 };
    ci.sType                    = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface                  = ctx->surface;
    ci.minImageCount            = image_count;
    ci.imageFormat              = ctx->surface_format.format;
    ci.imageColorSpace          = ctx->surface_format.colorSpace;
    ci.imageExtent              = extent;
    ci.imageArrayLayers         = 1;
    ci.imageUsage               = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform             = caps.currentTransform;
    ci.compositeAlpha           = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode              = ctx->present_mode;
    ci.clipped                  = VK_TRUE;
    ci.oldSwapchain             = old_swapchain;

    /* Exclusive mode when graphics and present share a queue family (most desktop HW). */
    u32 queue_families[ 2 ] = { vk.graphics_queue_family, vk.present_queue_family };    
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ( vk.graphics_queue_family != vk.present_queue_family )
    {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = queue_families;
    }

    r = vkCreateSwapchainKHR( vk.device, &ci, vk.alloc_cb, &ctx->swapchain );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "vkCreateSwapchainKHR: %s", string_VkResult( r ) );
        return false;
    }

    /* Retire the old swapchain now that the new one is live; driver may reuse its resources. */

    if ( old_swapchain != VK_NULL_HANDLE )
        vkDestroySwapchainKHR( vk.device, old_swapchain, vk.alloc_cb );

    /* --- Retrieve swapchain images (driver may give more than we requested) --- */

    /* Two-pass: query true count first so we can clamp before writing into the fixed-size array.
       Skipping this lets VK_INCOMPLETE go undetected and image_index OOB on acquire. */

    u32 true_image_count = 0;
    vkGetSwapchainImagesKHR( vk.device, ctx->swapchain, &true_image_count, NULL );
    if ( true_image_count > VK_MAX_SWAPCHAIN_IMAGES )
    {
        LOG_WARN( "swapchain_create: driver returned %u images; clamping to %u (ctx %d)",
                  true_image_count, VK_MAX_SWAPCHAIN_IMAGES, ctx->id );
        true_image_count = VK_MAX_SWAPCHAIN_IMAGES;
    }
    ctx->swapchain_image_count = true_image_count;
    vkGetSwapchainImagesKHR( vk.device, ctx->swapchain, &ctx->swapchain_image_count,
                             ctx->swapchain_images );

    /* --- Create image views --- */

    for ( u32 i = 0; i < ctx->swapchain_image_count; ++i )
    {
        VkImageViewCreateInfo view_ci           = { 0 };
        view_ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image                           = ctx->swapchain_images[ i ];
        view_ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format                          = ctx->surface_format.format;
        view_ci.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_ci.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_ci.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_ci.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.baseMipLevel   = 0;
        view_ci.subresourceRange.levelCount     = 1;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount     = 1;

        r = vkCreateImageView( vk.device, &view_ci, vk.alloc_cb, &ctx->swapchain_image_views[ i ] );
        if ( r != VK_SUCCESS ) {
             LOG_ERROR( "swapchain_create: vkCreateImageView[%u]: %s", i, string_VkResult( r ) );
             /* Destroy any views already created, then the swapchain. */
             for ( u32 j = 0; j < i; ++j )
                  vkDestroyImageView( vk.device, ctx->swapchain_image_views[ j ], vk.alloc_cb );
             vkDestroySwapchainKHR( vk.device, ctx->swapchain, vk.alloc_cb );
             ctx->swapchain = VK_NULL_HANDLE;
             return false;
        }
    }

    LOG_INFO( "vk_swapchain_create: OK (ctx %d, %ux%u, %u \n\t\timages,fmt=%s, mode=%s)",
              ctx->id, extent.width, extent.height, ctx->swapchain_image_count,
              string_VkFormat( ctx->surface_format.format ), 
              string_VkPresentModeKHR( ctx->present_mode ) );

    /* --- Create the depth buffer sized to match the swapchain --- */

    return vk_depth_create( ctx );
}

/*============================================================================================*/

static void
vk_swapchain_destroy( vk_context_t* ctx )
{
    vk_depth_destroy( ctx );

    for ( u32 i = 0; i < ctx->swapchain_image_count; ++i )
    {
        if ( ctx->swapchain_image_views[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyImageView( vk.device, ctx->swapchain_image_views[ i ], vk.alloc_cb );
            ctx->swapchain_image_views[ i ] = VK_NULL_HANDLE;
        }
    }
    ctx->swapchain_image_count = 0;

    if ( ctx->swapchain != VK_NULL_HANDLE ) {
         vkDestroySwapchainKHR( vk.device, ctx->swapchain, vk.alloc_cb );
         ctx->swapchain = VK_NULL_HANDLE;
    }
}

/*============================================================================================*/

static bool
vk_swapchain_recreate( vk_context_t* ctx )
{
    /* Query surface caps before touching anything.  On a minimized window the driver
       reports currentExtent {0,0}; skip recreation and let the caller retry next frame
       with the old swapchain still live. */

    LOG_LINE();

    VkSurfaceCapabilitiesKHR caps = { 0 };
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physical_device, ctx->surface, &caps );
    if ( caps.currentExtent.width == 0 || caps.currentExtent.height == 0 )
         return false;

    LOG_INFO( "swapchain_recreate: begin (ctx %d, %dx%d)", ctx->id, ctx->width, ctx->height );

    /* Save the old handle so we can pass it to vkCreateSwapchainKHR. The driver
       may reuse its presentation resources, avoiding a blank frame on resize. */

    VkSwapchainKHR old_swapchain = ctx->swapchain;
    ctx->swapchain               = VK_NULL_HANDLE;

    /* Wait only on this context's in-flight fences and the present queue; avoids draining 
       the whole device and stalling sibling contexts (e.g. other editor viewports).  */

    VkResult r = vkWaitForFences( vk.device, VK_MAX_FRAMES_IN_FLIGHT, ctx->in_flight_fence, VK_TRUE, UINT64_MAX );
    if ( r != VK_SUCCESS ) {
        LOG_ERROR( "swapchain_recreate: vkWaitForFences: %s", string_VkResult( r ) );
    }
    
    /* Waiting for the present queue before recreating is a synchronization step required
       to ensure that the Presentation Engine (the OS compositor or display hardware) 
       is finished with the old swapchain images before you start destroying or modifying them. */

    /* The old VkSwapchainKHR is kept alive and destroyed after the new swapchain is created */

    r = vkQueueWaitIdle( vk.present_queue );
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "swapchain_recreate: vkQueueWaitIdle: %s", string_VkResult( r ) );
    }

    vk_swapchain_destroy( ctx );

    bool ok = vk_swapchain_create( ctx, old_swapchain );
    LOG_INFO( "swapchain_recreate: %s (ctx %d)", ok ? "OK" : "FAILED", ctx->id );
    return ok;
}

/*==============================================================================================
    Depth buffer  (owned per context; recreated on resize along with the swapchain)
==============================================================================================*/

static bool
vk_depth_create( vk_context_t* ctx )
{
    /* Probe depth formats from most to least precise -- done once, shared across slots. */
    static const VkFormat s_candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT, 
        VK_FORMAT_D16_UNORM,
    };

    /* Vulkan uses a unified DEPTH_STENCIL bit for any depth/stencil pipeline usage,
       even for depth-only formats like D32 that don't have a stencil aspect. */

    ctx->depth_format = VK_FORMAT_UNDEFINED;
    for ( u32 i = 0; i < 3; ++i )
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties( vk.physical_device, s_candidates[ i ], &props );
        if ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT )
        {
            ctx->depth_format = s_candidates[ i ];
            break;
        }
    }
    if ( ctx->depth_format == VK_FORMAT_UNDEFINED ) {
         LOG_ERROR( "depth_create: no supported depth format (ctx %d)", ctx->id );
         return false;
    }

    /* Create one image + memory + view per frame-in-flight slot.
       On any failure, vk_depth_destroy cleans up all slots allocated so far. */
    for ( u32 slot = 0; slot < VK_MAX_FRAMES_IN_FLIGHT; ++slot )
    {
        VkImageCreateInfo img_ci      = { 0 };
        img_ci.sType                  = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType              = VK_IMAGE_TYPE_2D;
        img_ci.format                 = ctx->depth_format;
        img_ci.extent.width           = ctx->swapchain_extent.width;
        img_ci.extent.height          = ctx->swapchain_extent.height;
        img_ci.extent.depth           = 1;
        img_ci.mipLevels              = 1;
        img_ci.arrayLayers            = 1;
        img_ci.samples                = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling                 = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage                  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        img_ci.sharingMode            = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout          = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult r = vkCreateImage( vk.device, &img_ci, vk.alloc_cb, &ctx->depth_image[ slot ] );
        if ( r != VK_SUCCESS ) {
             LOG_ERROR( "depth_create: vkCreateImage[%u]: %s", slot, string_VkResult( r ) );
             vk_depth_destroy( ctx );
             return false;
        }

        VkMemoryRequirements reqs;
        vkGetImageMemoryRequirements( vk.device, ctx->depth_image[ slot ], &reqs );

        vk_mem_alloc_t alloc = { 0 };
        if ( !vk_mem_alloc( reqs, RHI_MEMORY_GPU_ONLY, 0, &alloc )) {
             vk_depth_destroy( ctx );
             return false;
        }
        ctx->depth_memory[ slot ] = alloc.memory;

        r = vkBindImageMemory( vk.device, ctx->depth_image[ slot ], ctx->depth_memory[ slot ], alloc.offset );
        if ( r != VK_SUCCESS ) {
             LOG_ERROR( "depth_create: vkBindImageMemory[%u]: %s", slot, string_VkResult( r ) );
             vk_depth_destroy( ctx );
             return false;
        }

        VkImageViewCreateInfo view_ci           = { 0 };
        view_ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image                           = ctx->depth_image[ slot ];
        view_ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format                          = ctx->depth_format;
        view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_ci.subresourceRange.baseMipLevel   = 0;
        view_ci.subresourceRange.levelCount     = 1;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount     = 1;

        r = vkCreateImageView( vk.device, &view_ci, vk.alloc_cb, &ctx->depth_view[ slot ] );
        if ( r != VK_SUCCESS ) {
             LOG_ERROR( "depth_create: vkCreateImageView[%u]: %s", slot, string_VkResult( r ) );
             vk_depth_destroy( ctx );
             return false;
        }

        /* Layout starts UNDEFINED; frame_begin barriers it to DEPTH_ATTACHMENT_OPTIMAL on first use. */
        ctx->depth_layout[ slot ] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    LOG_INFO( "vk_depth_create: OK (ctx %d, fmt=%s, %ux%u, %u slots)", ctx->id, 
                    string_VkFormat( ctx->depth_format ), ctx->swapchain_extent.width, 
                    ctx->swapchain_extent.height, (u32)VK_MAX_FRAMES_IN_FLIGHT );
    return true;
}

/*============================================================================================*/

static void
vk_depth_destroy( vk_context_t* ctx )
{
    for ( u32 slot = 0; slot < VK_MAX_FRAMES_IN_FLIGHT; ++slot )
    {
        if ( ctx->depth_view[ slot ] != VK_NULL_HANDLE )
        {
            vkDestroyImageView( vk.device, ctx->depth_view[ slot ], vk.alloc_cb );
            ctx->depth_view[ slot ] = VK_NULL_HANDLE;
        }
        if ( ctx->depth_image[ slot ] != VK_NULL_HANDLE )
        {
            vkDestroyImage( vk.device, ctx->depth_image[ slot ], vk.alloc_cb );
            ctx->depth_image[ slot ] = VK_NULL_HANDLE;
        }
        if ( ctx->depth_memory[ slot ] != VK_NULL_HANDLE )
        {
            vkFreeMemory( vk.device, ctx->depth_memory[ slot ], vk.alloc_cb );
            ctx->depth_memory[ slot ] = VK_NULL_HANDLE;
        }
        ctx->depth_layout[ slot ] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

/*============================================================================================*/
// clang-format on
