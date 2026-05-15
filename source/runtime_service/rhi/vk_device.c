/*==============================================================================================

    vulkan/vk_device.c — VkPhysicalDevice selection + VkDevice + queues.

    Called after vk_instance_create (needs the instance) and after the surface
    has been created in vk_swapchain.c (needs the surface to verify present
    support during physical-device selection).

==============================================================================================*/

static bool
vk_device_create( void )
{
    printf( "[rhi:vk] device_create (placeholder)\n" );

    /* TODO (Vulkan implementation):

       Physical device selection:
       - vkEnumeratePhysicalDevices( g_vk.instance, ... )
       - For each candidate:
            - Query VkPhysicalDeviceProperties (prefer DISCRETE_GPU)
            - Query VkPhysicalDeviceFeatures (require what we need)
            - Query queue families (find one supporting GRAPHICS + present)
            - vkGetPhysicalDeviceSurfaceSupportKHR for present support
            - Verify required device extensions are supported (VK_KHR_swapchain, etc.)
       - Pick the best candidate → g_vk.physical_device
       - Record g_vk.graphics_queue_family and g_vk.present_queue_family
         (often the same family; can be different on some hardware)

       Logical device:
       - Build VkDeviceQueueCreateInfo array (one per unique queue family)
       - Enable required device extensions (VK_KHR_swapchain, dynamic rendering, etc.)
       - Enable required features (synchronization2, dynamicRendering, etc. for VK 1.3)
       - vkCreateDevice → g_vk.device
       - vkGetDeviceQueue → g_vk.graphics_queue, g_vk.present_queue
       - Load device-level function pointers. */

    return true;
}

static void
vk_device_destroy( void )
{
    printf( "[rhi:vk] device_destroy (placeholder)\n" );

    /* TODO (Vulkan implementation):
       - vkDeviceWaitIdle before any destroy of dependent resources
       - vkDestroyDevice( g_vk.device, NULL )
       - Zero the handles. */
}

/*============================================================================================*/
