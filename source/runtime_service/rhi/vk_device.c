/*==============================================================================================

    vulkan/vk_device.c -- VkPhysicalDevice selection + VkDevice + queues.

    Called after vk_instance_create.  Physical device selection uses the platform
    presentation support query (vkGetPhysicalDeviceWin32PresentationSupportKHR etc.)
    as a proxy for surface support so no surface is needed at device-select time.
    The first context_create call validates the choice against a real surface.

==============================================================================================*/

#define VK_MAX_PHYSICAL_DEVICES 8
#define VK_MAX_QUEUE_FAMILIES   16
#define VK_QUEUE_FAMILY_INVALID ( ~0u )

/*==============================================================================================
    Physical device helpers
==============================================================================================*/

/* Returns true if dev exposes VK_KHR_swapchain.
   Sets *out_push_desc if VK_KHR_push_descriptor is also present. */

static bool
vk_device_check_extensions( VkPhysicalDevice dev, bool* out_push_desc )
{
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

/* Finds graphics+compute, present, and transfer queue families on dev.
   Returns false if the required gfx or present families cannot be satisfied. */

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

        if ( gfx == VK_QUEUE_FAMILY_INVALID &&
             ( flags & VK_QUEUE_GRAPHICS_BIT ) && ( flags & VK_QUEUE_COMPUTE_BIT ) )
        {
            gfx = i;
        }

        /* Use a platform surface-free query so no VkSurfaceKHR is needed here.
           context_create re-validates via vkGetPhysicalDeviceSurfaceSupportKHR. */

        if ( present == VK_QUEUE_FAMILY_INVALID )
        {
#if defined( VK_USE_PLATFORM_WIN32_KHR )
            if ( vkGetPhysicalDeviceWin32PresentationSupportKHR( dev, i ) )
                present = i;
#else
            /* Linux/Mac: no surface-free query; accept the first graphics family.
               context_create validates against a real surface. */

            if ( flags & VK_QUEUE_GRAPHICS_BIT )
                present = i;
#endif
        }

        /* Prefer a dedicated transfer family (no graphics or compute). */
        if ( transfer == VK_QUEUE_FAMILY_INVALID &&
             ( flags & VK_QUEUE_TRANSFER_BIT ) &&
             !( flags & VK_QUEUE_GRAPHICS_BIT ) &&
             !( flags & VK_QUEUE_COMPUTE_BIT ) )
        {
            transfer = i;
        }
    }

    /* Fall back to the graphics family when no dedicated transfer family exists. */
    if ( transfer == VK_QUEUE_FAMILY_INVALID )
        transfer = gfx;

    *out_gfx      = gfx;
    *out_present  = present;
    *out_transfer = transfer;

    return ( gfx != VK_QUEUE_FAMILY_INVALID ) && ( present != VK_QUEUE_FAMILY_INVALID );
}

/* Scores a physical device.  Returns -1 if the device fails any hard requirement. */
static int
vk_device_score( VkPhysicalDevice dev )
{
    bool push_desc_unused;
    if ( !vk_device_check_extensions( dev, &push_desc_unused ) )
        return -1;

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
    vk_device_create
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
        VkPhysicalDevice       dev   = candidates[ i ];
        int                    score = vk_device_score( dev );
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

    vkGetPhysicalDeviceProperties( vk.physical_device, &vk.physical_device_props );
    vkGetPhysicalDeviceMemoryProperties( vk.physical_device, &vk.memory_props );

    LOG_INFO( "selected: %s  (gfx=%u  present=%u  xfer=%u)",
              vk.physical_device_props.deviceName,
              vk.graphics_queue_family, vk.present_queue_family, vk.transfer_queue_family );

    /* --- Extension list for the logical device --- */

    bool        push_desc    = false;
    vk_device_check_extensions( vk.physical_device, &push_desc );

    const char* dev_exts[ 2 ];
    u32         dev_ext_count    = 0;
    dev_exts[ dev_ext_count++ ]  = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if ( push_desc )
    {
        dev_exts[ dev_ext_count++ ] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        LOG_INFO( "  VK_KHR_push_descriptor: enabled" );
    }

    /* --- VK 1.3 feature chain (pNext from bottom to top) --- */

    VkPhysicalDeviceDescriptorIndexingFeatures desc_idx = { 0 };
    desc_idx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    desc_idx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    desc_idx.descriptorBindingPartiallyBound           = VK_TRUE;
    desc_idx.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    desc_idx.runtimeDescriptorArray                    = VK_TRUE;

    VkPhysicalDeviceDynamicRenderingFeatures dyn_rend = { 0 };
    dyn_rend.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dyn_rend.pNext            = &desc_idx;
    dyn_rend.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceSynchronization2Features sync2 = { 0 };
    sync2.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2.pNext            = &dyn_rend;
    sync2.synchronization2 = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = { 0 };
    timeline.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.pNext             = &sync2;
    timeline.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceFeatures2 feats2 = { 0 };
    feats2.sType                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext                      = &timeline;
    feats2.features.samplerAnisotropy = VK_TRUE;
    feats2.features.fillModeNonSolid  = VK_TRUE;

    /* --- Queue create infos (deduplicate family indices) --- */

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
       NOTE: pEnabledFeatures must be NULL when VkPhysicalDeviceFeatures2 is in pNext. */

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

    /* Load device-level function pointers before making any further device calls. */
    if ( !vk_lib_device_entry_points() )
    {
        vkDestroyDevice( vk.device, vk.alloc_cb );
        vk.device = VK_NULL_HANDLE;
        return false;
    }

    /* --- Retrieve queue handles --- */

    vkGetDeviceQueue( vk.device, vk.graphics_queue_family, 0, &vk.graphics_queue );
    vkGetDeviceQueue( vk.device, vk.present_queue_family,  0, &vk.present_queue  );
    vkGetDeviceQueue( vk.device, vk.transfer_queue_family, 0, &vk.transfer_queue  );

    /* --- Subsystem init --- */

    vk_pipeline_cache_load();

    if ( !vk_descriptor_init() )
        goto fail_after_cache;

    if ( !vk_upload_init() )
        goto fail_after_descriptor;

    LOG_INFO( "device_create: OK" );
    return true;

fail_after_descriptor:
    vk_descriptor_shutdown();
fail_after_cache:
    vk_pipeline_cache_save();
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

    vkDeviceWaitIdle( vk.device );

    vk_upload_shutdown();
    vk_descriptor_shutdown();

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
