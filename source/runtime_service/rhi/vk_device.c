/*==============================================================================================

    vulkan/vk_device.c -- VkPhysicalDevice selection + VkDevice + queues.

    Called after vk_instance_create.  Physical device selection uses the platform
    presentation support query (vkGetPhysicalDeviceWin32PresentationSupportKHR etc.)
    as a proxy for surface support so no surface is needed at device-select time.
    The first context_create call validates the choice against a real surface.

    Required Vulkan 1.3 features enabled here:
        dynamicRendering     -- render passes without VkRenderPass / VkFramebuffer
        synchronization2     -- cleaner pipeline barrier API (VkDependencyInfo)
        timelineSemaphore    -- multi-frame GPU/CPU sync without polling

    Required Vulkan 1.2 features enabled here:
        descriptorIndexing   -- bindless textures/samplers in set 0; shaders index them
                                with push-constant indices instead of per-draw binds.
                                Specific flags:
                                    shaderSampledImageArrayNonUniformIndexing -- dynamic
                                        index into the texture array per-invocation
                                    descriptorBindingPartiallyBound -- slots in the bindless
                                        array may be empty; shaders must not access them
                                    descriptorBindingUpdateUnusedWhilePending -- allows
                                        writing new descriptors while GPU uses other slots
                                    runtimeDescriptorArray -- arrays with size set at
                                        pipeline-layout time rather than shader-compile time
        timelineSemaphore    -- promoted to core in 1.2; declared in 1.2 feature struct

    Queue layout expected by the rest of the RHI:
        graphics_queue  -- all draw calls, dynamic rendering, image layout transitions
        present_queue   -- vkQueuePresentKHR; often the same handle as graphics on desktop
        transfer_queue  -- async staging uploads; separate family reduces bubbles on AMD/NVIDIA
                           discrete transfer hardware; falls back to graphics if not available

==============================================================================================*/
// clang-format off

#define VK_MAX_PHYSICAL_DEVICES 8
#define VK_MAX_QUEUE_FAMILIES   16
#define VK_QUEUE_FAMILY_INVALID ( ~0u )

/*==============================================================================================
    Physical device helpers
==============================================================================================*/

/* Returns true if dev exposes VK_KHR_swapchain (hard requirement: no swapchain = no window).
   Sets *out_push_desc if VK_KHR_push_descriptor is also available; that extension is
   optional -- the RHI uses it for efficient per-draw descriptor updates when present. */

static bool
vk_device_check_extensions( VkPhysicalDevice dev, bool* out_push_desc )
{
    /* Two-call pattern: first get count, then fill.  Device extension counts can reach
       200+ on current drivers, so malloc rather than risk a large stack allocation. */
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, NULL );

    VkExtensionProperties* exts = malloc( sizeof( VkExtensionProperties ) * ( count + 1 ) );
    if ( !exts )
        return false;

    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, exts );

    bool has_swapchain = false;
    bool has_push_desc = false;

    for ( u32 i = 0; i < count; ++i )
    {
        const char* name = exts[ i ].extensionName;
        if ( strcmp( name, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 )
            has_swapchain = true;
        if ( strcmp( name, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME ) == 0 )
            has_push_desc = true;
    }

    free( exts );
    *out_push_desc = has_push_desc;
    return has_swapchain;
}

/* Walks queue families and fills three indices: graphics+compute, present, transfer.
   Returns false only if the hard requirements (gfx or present) cannot be satisfied.

   Why combined graphics+compute family:
       The render loop issues graphics and compute work on the same timeline; splitting
       them across families would require explicit ownership transfers and extra sync.

   Why dedicated transfer:
       AMD and NVIDIA expose a copy-only family backed by DMA hardware that runs in
       parallel with the graphics engine.  Routing staging uploads through it avoids
       pipeline stalls on the graphics queue during asset streaming.

   Why surface-free present query:
       vkGetPhysicalDeviceSurfaceSupportKHR requires a live VkSurfaceKHR.  The platform
       query (Win32/Xlib/Wayland variant) answers without one so device selection can
       happen before any window is open.  context_create re-validates against a real
       surface via vkGetPhysicalDeviceSurfaceSupportKHR before committing. */

static bool
vk_device_find_queues( VkPhysicalDevice dev,
                       u32* out_gfx, u32* out_present, u32* out_transfer )
{
    VkQueueFamilyProperties families[ VK_MAX_QUEUE_FAMILIES ];
    u32 count = VK_MAX_QUEUE_FAMILIES;
    vkGetPhysicalDeviceQueueFamilyProperties( dev, &count, families );

    u32 gfx      = VK_QUEUE_FAMILY_INVALID;
    u32 present  = VK_QUEUE_FAMILY_INVALID;
    u32 transfer = VK_QUEUE_FAMILY_INVALID;

    for ( u32 i = 0; i < count; ++i )
    {
        VkQueueFlags flags = families[ i ].queueFlags;

        /* Require both GRAPHICS and COMPUTE in the same family so compute shaders
           (post-processing, particle simulation) can share the same command buffer
           and synchronisation scope as draw calls without cross-family barriers. */
        if ( gfx == VK_QUEUE_FAMILY_INVALID &&
             ( flags & VK_QUEUE_GRAPHICS_BIT ) && ( flags & VK_QUEUE_COMPUTE_BIT ) )
        {
            gfx = i;
        }

        if ( present == VK_QUEUE_FAMILY_INVALID )
        {
#if defined( VK_USE_PLATFORM_WIN32_KHR )
            /* Win32: surface-free query -- works without a real HWND/VkSurfaceKHR.
               Most Win32 drivers expose present support on the graphics family. */
            if ( vkGetPhysicalDeviceWin32PresentationSupportKHR( dev, i ) )
                present = i;
#else
            /* Linux/Mac: no surface-free present query exists in the Vulkan spec
               without platform extensions that require a live display connection.
               Accept any graphics family; context_create will confirm with
               vkGetPhysicalDeviceSurfaceSupportKHR once a surface exists. */
            if ( flags & VK_QUEUE_GRAPHICS_BIT )
                present = i;
#endif
        }

        /* Dedicated transfer family: TRANSFER bit set, but not GRAPHICS or COMPUTE.
           On discrete AMD/NVIDIA this maps to DMA hardware that runs asynchronously
           alongside the 3D engine, enabling zero-stall staging uploads. */
        if ( transfer == VK_QUEUE_FAMILY_INVALID &&
             ( flags & VK_QUEUE_TRANSFER_BIT ) &&
             !( flags & VK_QUEUE_GRAPHICS_BIT ) &&
             !( flags & VK_QUEUE_COMPUTE_BIT ) )
        {
            transfer = i;
        }
    }

    /* No dedicated transfer hardware (integrated GPUs, older cards): route uploads
       through the graphics family.  vk_upload.c submits on transfer_queue regardless,
       so the fallback is transparent to the rest of the code. */
    if ( transfer == VK_QUEUE_FAMILY_INVALID )
        transfer = gfx;

    *out_gfx      = gfx;
    *out_present  = present;
    *out_transfer = transfer;

    return ( gfx != VK_QUEUE_FAMILY_INVALID ) && ( present != VK_QUEUE_FAMILY_INVALID );
}

/*==============================================================================================
    Scores a physical device for selection.  Returns -1 on any hard requirement failure,
    otherwise a positive integer.  Higher is better; caller picks the maximum.

   Score bands:
       1000 -- discrete GPU (dedicated VRAM; ideal for a game engine)
        100 -- integrated GPU (shared system memory; acceptable fallback)
          1 -- virtual / software / unknown (CI machines, WSL2, RenderDoc captures)
==============================================================================================*/

static int
vk_device_score( VkPhysicalDevice dev )
{
    bool push_desc_unused;
    if ( !vk_device_check_extensions( dev, &push_desc_unused ) )
        return -1;

    /* samplerAnisotropy: required for correct high-quality texture filtering on
       surfaces at oblique angles (terrain, floors). */ 
       
    /* fillModeNonSolid: required
       for wireframe debug overlays (VK_POLYGON_MODE_LINE in pipeline state). */

    VkPhysicalDeviceFeatures2 feats2 = { 0 };
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vkGetPhysicalDeviceFeatures2( dev, &feats2 );
    if ( !feats2.features.samplerAnisotropy || !feats2.features.fillModeNonSolid )
        return -1;

    u32 gfx, present, transfer;
    if ( !vk_device_find_queues( dev, &gfx, &present, &transfer ) )
        return -1;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties( dev, &props );

    if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ) return 1000;
    if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) return  100;
    return 1;
}

/*==============================================================================================
    Main device creation function. Called once at global init time to pick a physical
    device, then create a logical VkDevice and queues.
==============================================================================================*/

static bool
vk_device_create( void )
{
    /* --- Enumerate physical devices --- */

    VkPhysicalDevice candidates[ VK_MAX_PHYSICAL_DEVICES ];
    u32 device_count = VK_MAX_PHYSICAL_DEVICES;

    VkResult result = vkEnumeratePhysicalDevices( vk.instance, &device_count, candidates );
    if (( result != VK_SUCCESS && result != VK_INCOMPLETE ) || device_count == 0 )
    {
        LOG_ERROR( "vkEnumeratePhysicalDevices: %s (count=%u)", string_VkResult( result ), device_count );
        return false;
    }

    /* --- Score and select best candidate --- */

    VkPhysicalDevice best       = VK_NULL_HANDLE;
    u32              best_gfx   = VK_QUEUE_FAMILY_INVALID;
    u32              best_pres  = VK_QUEUE_FAMILY_INVALID;
    u32              best_xfer  = VK_QUEUE_FAMILY_INVALID;
    int              best_score = -1;

    for ( u32 i = 0; i < device_count; ++i )
    {
        VkPhysicalDevice dev   = candidates[ i ];
        int              score = vk_device_score( dev );

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties( dev, &props );
        LOG_INFO( "  gpu[%u]: %-40s  score=%d", i, props.deviceName, score );

        if ( score > best_score )
        {
            u32 gfx, pres, xfer;
            vk_device_find_queues( dev, &gfx, &pres, &xfer );
            best       = dev;
            best_score = score;
            best_gfx   = gfx;
            best_pres  = pres;
            best_xfer  = xfer;
        }
    }

    if ( best == VK_NULL_HANDLE )
    {
        LOG_ERROR( "no suitable physical device found "
                   "(requires VK_KHR_swapchain, samplerAnisotropy, fillModeNonSolid, "
                   "graphics+compute queue, present support)" );
        return false;
    }

    vk.physical_device       = best;
    vk.graphics_queue_family = best_gfx;
    vk.present_queue_family  = best_pres;
    vk.transfer_queue_family = best_xfer;

    /* Store physical device properties globally; other subsystems read them at init time
       (e.g. vk_memory.c checks limits.maxMemoryAllocationCount, vk_texture.c reads
       limits.maxImageDimension2D). */

    vkGetPhysicalDeviceProperties( vk.physical_device, &vk.physical_device_props );
    vkGetPhysicalDeviceMemoryProperties( vk.physical_device, &vk.memory_props );

    LOG_INFO( "selected: %s  (gfx=%u  present=%u  xfer=%u)",
              vk.physical_device_props.deviceName,
              vk.graphics_queue_family, vk.present_queue_family, vk.transfer_queue_family );

    /* --- Extension list for the logical device ---
       Re-check the winner rather than caching results from the scoring pass; the
       scoring loop may have evaluated multiple devices, and we only want the winner's
       extension set reflected in the device create info. */

    bool        push_desc    = false;
    vk_device_check_extensions( vk.physical_device, &push_desc );

    const char* dev_exts[ 2 ];
    u32         dev_ext_count    = 0;
    dev_exts[ dev_ext_count++ ]  = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if ( push_desc )
    {
        /* VK_KHR_push_descriptor: lets vkCmdPushDescriptorSetKHR update a small set
           of descriptors inline in the command buffer -- avoids pool/set management for
           per-draw data that changes every frame. */
        dev_exts[ dev_ext_count++ ] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        LOG_INFO( "  VK_KHR_push_descriptor: enabled" );
    }

    /* --- VK 1.3 feature chain (pNext built bottom to top) ---
       Each struct's pNext points to the one declared before it so the chain reads
       desc_idx -> dyn_rend -> sync2 -> timeline -> feats2 (root).
       Vulkan walks the chain from feats2.pNext when processing vkCreateDevice. */

    VkPhysicalDeviceDescriptorIndexingFeatures desc_idx = { 0 };
    desc_idx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    desc_idx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;  /* dynamic index in shader */
    desc_idx.descriptorBindingPartiallyBound           = VK_TRUE;  /* sparse bindless slots ok */
    desc_idx.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;  /* write while GPU reads others */
    desc_idx.runtimeDescriptorArray                    = VK_TRUE;  /* array size from layout, not SPIR-V */

    VkPhysicalDeviceDynamicRenderingFeatures dyn_rend = { 0 };
    dyn_rend.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dyn_rend.pNext            = &desc_idx;
    dyn_rend.dynamicRendering = VK_TRUE;  /* vkCmdBeginRendering; no VkRenderPass objects */

    VkPhysicalDeviceSynchronization2Features sync2 = { 0 };
    sync2.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2.pNext            = &dyn_rend;
    sync2.synchronization2 = VK_TRUE;  /* vkCmdPipelineBarrier2 / vkQueueSubmit2 */

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = { 0 };
    timeline.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.pNext             = &sync2;
    timeline.timelineSemaphore = VK_TRUE;  /* GPU/CPU sync with monotonically increasing counter */

    VkPhysicalDeviceFeatures2 feats2 = { 0 };
    feats2.sType                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext                      = &timeline;          /* root of the chain above */
    feats2.features.samplerAnisotropy = VK_TRUE;            /* anisotropic texture filtering */
    feats2.features.fillModeNonSolid  = VK_TRUE;            /* wireframe debug polygon mode */

    /* --- Queue create infos (deduplicate family indices) ---
       Vulkan requires exactly one VkDeviceQueueCreateInfo per unique family index.
       On most desktop hardware graphics == present; transfer may or may not be separate.
       The dedup loop ensures we never pass duplicate families to vkCreateDevice,
       which would be a validation error.

       Priority: graphics and present get 1.0 (highest); transfer gets 0.5 when it is a
       separate family to hint the driver it is lower-priority than rendering.  When
       transfer shares the graphics family the priority has no effect since only one
       VkDeviceQueueCreateInfo is emitted for that family. */

    float prio_high = 1.0f;
    float prio_low  = 0.5f;

    u32                     req_families[ 3 ] = { vk.graphics_queue_family,
                                                   vk.present_queue_family,
                                                   vk.transfer_queue_family };
    float*                  req_prio[ 3 ]     = { &prio_high, &prio_high, &prio_low };
    u32                     seen[ 3 ]         = { VK_QUEUE_FAMILY_INVALID,
                                                   VK_QUEUE_FAMILY_INVALID,
                                                   VK_QUEUE_FAMILY_INVALID };
    VkDeviceQueueCreateInfo qcis[ 3 ];
    u32                     qci_count = 0;

    for ( u32 i = 0; i < 3; ++i )
    {
        u32  fam = req_families[ i ];
        bool dup = false;
        for ( u32 j = 0; j < qci_count; ++j )
        {
            if ( seen[ j ] == fam ) { dup = true; break; }
        }
        if ( dup )
            continue;

        seen[ qci_count ]                  = fam;
        qcis[ qci_count ]                  = ( VkDeviceQueueCreateInfo ){ 0 };
        qcis[ qci_count ].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[ qci_count ].queueFamilyIndex = fam;
        qcis[ qci_count ].queueCount       = 1;
        qcis[ qci_count ].pQueuePriorities = req_prio[ i ];
        ++qci_count;
    }

    /* --- Logical device creation ---
       pEnabledFeatures MUST be NULL when VkPhysicalDeviceFeatures2 is in pNext.
       Setting both is a validation error; features2 is the authoritative path for
       Vulkan 1.1+ and is required to chain the extension feature structs above. */

    VkDeviceCreateInfo dev_ci          = { 0 };
    dev_ci.sType                       = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_ci.pNext                       = &feats2;
    dev_ci.queueCreateInfoCount        = qci_count;
    dev_ci.pQueueCreateInfos           = qcis;
    dev_ci.enabledExtensionCount       = dev_ext_count;
    dev_ci.ppEnabledExtensionNames     = dev_exts;

    result = vkCreateDevice( vk.physical_device, &dev_ci, vk.alloc_cb, &vk.device );
    if ( result != VK_SUCCESS )
    {
        LOG_ERROR( "vkCreateDevice: %s", string_VkResult( result ) );
        return false;
    }

    /* Device-level function pointers must be loaded before any vkCmd* or resource
       calls.  vk_lib_device_entry_points uses vkGetDeviceProcAddr(vk.device, ...)
       so vk.device must be live before calling it. */
    if ( !vk_lib_device_entry_points() )
    {
        vkDestroyDevice( vk.device, vk.alloc_cb );
        vk.device = VK_NULL_HANDLE;
        return false;
    }

    /* --- Retrieve queue handles ---
       All three may resolve to the same VkQueue object when families overlap; that is
       valid -- the driver serialises them internally. */

    vkGetDeviceQueue( vk.device, vk.graphics_queue_family, 0, &vk.graphics_queue );
    vkGetDeviceQueue( vk.device, vk.present_queue_family,  0, &vk.present_queue  );
    vkGetDeviceQueue( vk.device, vk.transfer_queue_family, 0, &vk.transfer_queue  );

    /* --- Subsystem init ---
       Order matters: pipeline cache before descriptor (cache handle is passed to
       vkCreateGraphicsPipelines later); descriptor before upload (upload records
       transfer commands that assume the pipeline layout is live). */

    vk_pipeline_cache_load();

    if ( !vk_descriptor_init() )
        goto fail_after_cache;

    if ( !vk_upload_init() )
        goto fail_after_descriptor;

    LOG_INFO( "device_create: OK" );
    return true;

fail_after_descriptor:  vk_descriptor_shutdown();
fail_after_cache:       vk_pipeline_cache_save();

    if ( vk.pipeline_cache != VK_NULL_HANDLE )
    {
        vkDestroyPipelineCache( vk.device, vk.pipeline_cache, vk.alloc_cb );
        vk.pipeline_cache = VK_NULL_HANDLE;
    }
    vkDestroyDevice( vk.device, vk.alloc_cb );
    vk.device = VK_NULL_HANDLE;
    return false;
}

/*==============================================================================================
    vk_device_destroy
==============================================================================================*/

static void
vk_device_destroy( void )
{
    if ( vk.device == VK_NULL_HANDLE )
        return;

    /* Drain all queues before touching any device-owned objects.  Skipping this causes
       validation errors and potential crashes if the GPU is still executing work. */
    vkDeviceWaitIdle( vk.device );

    /* Reverse init order: upload holds staging buffers that reference device memory,
       so it must be torn down before descriptor (which owns the pipeline layout), which
       must be torn down before the device itself. */
    vk_upload_shutdown();
    vk_descriptor_shutdown();

    /* Serialize the pipeline cache to disk so the next run can skip shader compilation
       work the driver already performed this session. */
    vk_pipeline_cache_save();
    if ( vk.pipeline_cache != VK_NULL_HANDLE )
    {
        vkDestroyPipelineCache( vk.device, vk.pipeline_cache, vk.alloc_cb );
        vk.pipeline_cache = VK_NULL_HANDLE;
    }

    vkDestroyDevice( vk.device, vk.alloc_cb );
    vk.device = VK_NULL_HANDLE;
}

/*============================================================================================*/
// clang-format on