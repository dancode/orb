/*==============================================================================================

    vulkan/vk_device.c -- VkPhysicalDevice selection + VkDevice + queues.

    Called after vk_instance_create.  Physical device selection uses the platform
    presentation support query (vkGetPhysicalDeviceWin32PresentationSupportKHR etc.)
    as a proxy for surface support so no surface is needed at device-select time.
    The first context_create call validates the choice against a real surface.

==============================================================================================*/

static bool
vk_device_create( void )
{
    LOG_INFO( "device_create (placeholder)" );

    /* TODO (Vulkan implementation):

       --- Physical device selection ---

       vkEnumeratePhysicalDevices( g_vk.instance, &count, devices )

       For each candidate:
           vkGetPhysicalDeviceProperties2( dev, &props2 )
               -- prefer VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
           vkGetPhysicalDeviceFeatures2( dev, &feats2 )
               -- require: samplerAnisotropy, fillModeNonSolid
               -- require: features from VK 1.2 + 1.3 (see below)
           vkGetPhysicalDeviceQueueFamilyProperties( dev, &qfc, qfamilies )
               -- find family with GRAPHICS + COMPUTE bits
               -- find family supporting present (platform query)
               -- find family with TRANSFER bit (prefer dedicated; fallback = graphics)
           vkEnumerateDeviceExtensionProperties
               -- require: VK_KHR_SWAPCHAIN_EXTENSION_NAME
               -- optional: VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
           Record g_vk.graphics_queue_family, present_queue_family, transfer_queue_family

       Pick best candidate -> g_vk.physical_device
       vkGetPhysicalDeviceMemoryProperties -> g_vk.memory_props
       vkGetPhysicalDeviceProperties       -> g_vk.physical_device_props

       --- Logical device (VK 1.3 feature chain) ---

       Chain pNext from bottom to top so each sType is valid:

       VkPhysicalDeviceDescriptorIndexingFeatures desc_idx = {
           .sType                                     = ...DESCRIPTOR_INDEXING_FEATURES,
           .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
           .descriptorBindingPartiallyBound           = VK_TRUE,
           .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
           .runtimeDescriptorArray                    = VK_TRUE,
       };
       VkPhysicalDeviceDynamicRenderingFeatures dyn_rend = {
           .sType            = ...DYNAMIC_RENDERING_FEATURES,
           .pNext            = &desc_idx,
           .dynamicRendering = VK_TRUE,
       };
       VkPhysicalDeviceSynchronization2Features sync2 = {
           .sType              = ...SYNCHRONIZATION_2_FEATURES,
           .pNext              = &dyn_rend,
           .synchronization2  = VK_TRUE,
       };
       VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
           .sType             = ...TIMELINE_SEMAPHORE_FEATURES,
           .pNext             = &sync2,
           .timelineSemaphore = VK_TRUE,
       };
       VkPhysicalDeviceFeatures2 feats2 = {
           .sType = ...PHYSICAL_DEVICE_FEATURES_2,
           .pNext = &timeline,
           .features = { .samplerAnisotropy = VK_TRUE, ... },
       };

       Build VkDeviceQueueCreateInfo[] for each unique queue family (one entry per family).
       Use a priority of 1.0f for the graphics queue; 0.5f for transfer if separate.

       VkDeviceCreateInfo dev_ci = {
           .pNext                   = &feats2,
           .queueCreateInfoCount    = ...,
           .pQueueCreateInfos       = qci,
           .enabledExtensionCount   = ...,
           .ppEnabledExtensionNames = exts,
       };
       -- NOTE: do NOT set pEnabledFeatures when using VkPhysicalDeviceFeatures2 in pNext

       vkCreateDevice -> g_vk.device

       vk_lib_device_entry_points()   -- load all device-level function pointers

       vkGetDeviceQueue -> g_vk.graphics_queue, present_queue, transfer_queue

       vk_pipeline_cache_load()       -- restore serialized cache from disk if present
       vk_descriptor_init()           -- create bindless pool + layout + set + pipeline layout
       vk_upload_init()               -- allocate per-frame staging buffers
    */

    return true;
}

static void
vk_device_destroy( void )
{
    LOG_INFO( "device_destroy (placeholder)" );

    /* TODO (Vulkan implementation):
       vkDeviceWaitIdle( g_vk.device )
       vk_upload_shutdown()
       vk_descriptor_shutdown()
       vk_pipeline_cache_save()       -- serialize cache to disk
       vkDestroyPipelineCache( g_vk.device, g_vk.pipeline_cache, g_vk.alloc_cb )
       vkDestroyDevice( g_vk.device, g_vk.alloc_cb )
       g_vk.device = VK_NULL_HANDLE
    */
}

/*============================================================================================*/
