/*==============================================================================================

    vulkan/vk_instance.c — VkInstance + debug messenger.

    First thing rhi_init() calls. Owns the connection to the Vulkan loader and
    the validation diagnostic plumbing in debug builds.

==============================================================================================*/

static bool
vk_instance_create( void )
{
    printf( "[rhi:vk] instance_create (placeholder)\n" );

    /* TODO (Vulkan implementation):
       1. Fill VkApplicationInfo with engine name, version, apiVersion = VK_API_VERSION_1_3.
       2. Query supported instance extensions via vkEnumerateInstanceExtensionProperties.
       3. Build required-extensions list:
            - VK_KHR_surface
            - VK_KHR_win32_surface  (or platform equivalent)
            - VK_EXT_debug_utils    (debug builds only)
       4. Build required-layers list:
            - VK_LAYER_KHRONOS_validation  (debug builds only, if available)
       5. vkCreateInstance → g_vk.instance.
       6. Load instance-level function pointers (volk, or manual vkGetInstanceProcAddr).
       7. In debug, vkCreateDebugUtilsMessengerEXT with a callback that routes
          severity≥WARNING through printf for now (later: core()->log_warn). */

    return true;
}

static void
vk_instance_destroy( void )
{
    printf( "[rhi:vk] instance_destroy (placeholder)\n" );

    /* TODO (Vulkan implementation):
       - vkDestroyDebugUtilsMessengerEXT( g_vk.instance, g_vk.debug_messenger, NULL )
       - vkDestroyInstance( g_vk.instance, NULL )
       - Zero the handles in g_vk. */
}

/*============================================================================================*/
