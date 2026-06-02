/*==============================================================================================

    vulkan/vk_instance.c -- VkInstance + debug messenger.

    First step in global init.  Owns the connection to the Vulkan loader and
    the validation diagnostic plumbing in debug builds.

==============================================================================================*/

static bool
vk_instance_create( void )
{
    printf( "[rhi:vk] instance_create (placeholder)\n" );

    /* TODO (Vulkan implementation):

       1. Verify Vulkan 1.3 is available:
              vkEnumerateInstanceVersion( &ver );
              if ( VK_API_VERSION_MINOR(ver) < 3 ) -- fail with message

       2. Fill VkApplicationInfo:
              sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO
              pEngineName      = "orb"
              engineVersion    = VK_MAKE_API_VERSION( 0, ORB_VERSION_MAJOR, ... )
              apiVersion       = VK_API_VERSION_1_3

       3. Query supported instance extensions via vkEnumerateInstanceExtensionProperties.
          Build required extension list:
              VK_KHR_SURFACE_EXTENSION_NAME
              platform surface (VK_KHR_WIN32_SURFACE_EXTENSION_NAME etc.)
              VK_EXT_DEBUG_UTILS_EXTENSION_NAME  (debug builds only; skip if absent)

       4. Query supported layers via vkEnumerateInstanceLayerProperties.
          Optionally enable:
              "VK_LAYER_KHRONOS_validation"  (debug builds; skip if absent)

       5. vkCreateInstance -> g_vk.instance

       6. Load instance-level function pointers:
              vk_lib_instance_entry_points()

       7. In debug builds, create the debug messenger:
              vk_debug_messenger_create()  (defined in vk_debug.c)
    */

    return true;
}

static void
vk_instance_destroy( void )
{
    printf( "[rhi:vk] instance_destroy (placeholder)\n" );

    /* TODO (Vulkan implementation):
       #if DEBUG
           vk_debug_messenger_destroy()
       #endif
       vkDestroyInstance( g_vk.instance, g_vk.alloc_cb )
       g_vk.instance = VK_NULL_HANDLE
    */
}

/*============================================================================================*/
