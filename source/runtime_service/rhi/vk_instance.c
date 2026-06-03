/*==============================================================================================

    vulkan/vk_instance.c -- VkInstance + debug messenger.

    First step in global init.  Owns the connection to the Vulkan loader and
    the validation diagnostic plumbing in debug builds.

==============================================================================================*/
// clang-format off
/*==============================================================================================

    Vulkan extensions provide additional functionality beyond the core Vulkan API .
    We use this function to acquire our required vulkan extentions.

==============================================================================================*/

static i32
vk_instance_get_extensions( const char** out_ext_array )
{
    i32                   out_ext_count   = 0;
    u32                   ext_count       = 0;
    VkExtensionProperties ext_props[ 32 ] = { 0 };

    UNUSED( ext_props );
    UNUSED( ext_count );

    /* collect list of vulkan extentions */
    
    VkResult result = VK_SUCCESS;
    result = vkEnumerateInstanceExtensionProperties( NULL, &ext_count, NULL );
    result = vkEnumerateInstanceExtensionProperties( NULL, &ext_count, ext_props );
    
    assert( ext_count <= 32 );
    
    if ( result != VK_SUCCESS )
    {
        LOG_ERROR( "vkEnumerateInstanceExtensionProperties: %s", string_VkResult( result ) );
        return 0;
    }
    
    /* validate the extentions we require exists. */
    
    for ( u32 i = 0; i < ext_count; i++ )
    {
        const char* ext_str = ext_props[ i ].extensionName;
        if ( strcmp( VK_KHR_WIN32_SURFACE_EXTENSION_NAME, ext_str ) == 0 ) 
        {
            vk.ext_win32_surface = true;
        }
        if ( strcmp( VK_KHR_SURFACE_EXTENSION_NAME, ext_str ) == 0 )
        {
            vk.ext_khr_surface = true;
        }
    }
    
    if ( vk.ext_win32_surface == false || vk.ext_khr_surface == false )
    {
        LOG_ERROR( "failed to get win32 surface extensions" );
        return 0;
    }
    
    /* add the extentions to extention list */
    
    out_ext_array[ 0 ] = VK_KHR_SURFACE_EXTENSION_NAME;
    out_ext_array[ 1 ] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    out_ext_count = 2;
    
    if ( vk.use_vk_debug_messenger )
    {
        out_ext_array[ 2 ] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        out_ext_count++;
    }
    
    LOG_INFO( "VK: %u extensions supported", ext_count );
    for ( u32 i = 0; i < ext_count; i++ )
    {
        LOG_DEBUG( "VKEXT: %s", ext_props[ i ].extensionName );
    }

    return out_ext_count;
}

/*==============================================================================================

    Vulkan layers are optional components that can provide additional functionality or
    validation for Vulkan applications. We use this function to acquire our layers.

==============================================================================================*/

static i32 /* count */
vk_instance_get_layers( const char** out_layer_names )
{
    i32 out_layer_count = 0;

    /* get the instance support layers list. */
    
    u32               layer_count = 0;
    VkLayerProperties layer_props[ 32 ] = { 0 };
    
    /* get available vulkan layers */
    
    VkResult result = VK_SUCCESS;
    result = vkEnumerateInstanceLayerProperties( &layer_count, NULL );
    result = vkEnumerateInstanceLayerProperties( &layer_count, layer_props );
    if ( result != VK_SUCCESS )
    {
        LOG_ERROR( "vkEnumerateInstanceLayerProperties: %s", string_VkResult( result ) );
        return 0;
    }
    
    assert( layer_count <= 32 );
    
    /* set all the instance layers we require */

    static const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    static const char* monitor_layer    = "VK_LAYER_LUNARG_monitor";
    
    bool has_layer_validation = false;
    bool has_layer_monitor    = false;
    
    /* validate debug layer we require exists in layer list. */
    
    for ( u32 i = 0; i < layer_count; i++ )
    {   
        if ( strcmp( validation_layer, layer_props[ i ].layerName ) == 0 )
            has_layer_validation = true;

        if ( strcmp( monitor_layer, layer_props[ i ].layerName ) == 0 )
            has_layer_monitor = true;
    }
    
    /*  only enable if we are using validation and monitoring */
    
    if ( vk.use_vk_layer_validation && has_layer_validation )
    {
        out_layer_names[ out_layer_count ] = validation_layer;
        out_layer_count++;
    }
    else
    {
        vk.use_vk_layer_validation = false;
    }
    if ( vk.use_vk_layer_monitor && has_layer_monitor )
    {
        out_layer_names[ out_layer_count ] = monitor_layer;
        out_layer_count++;
    }
    else
    {
        vk.use_vk_layer_monitor = false;
    }

    for ( u32 i = 0; i < layer_count; i++ )
    {
        LOG_DEBUG( "VKLYR: %s", layer_props[ i ].layerName );    
    }

    return out_layer_count;
}

/*==============================================================================================
    VK: Acquire our version of Vulkan
==============================================================================================*/

static i32
vk_get_version()
{
    u32 api_version = 0;
    VkResult r = vkEnumerateInstanceVersion( &api_version );
    if ( r == VK_SUCCESS )
    {
        u32 major = VK_VERSION_MAJOR( api_version );
        u32 minor = VK_VERSION_MINOR( api_version );
        u32 patch = VK_VERSION_PATCH( api_version );

        LOG_INFO( "vulkan api version: %d.%d.%d", major, minor, patch );
        return minor;
    }
    else
    {
        LOG_INFO( "failed to query vulkan api version" );
        return 0;
    }
}

/*==============================================================================================
    Global lifecycle  (instance + device, no window)
==============================================================================================*/

static bool
vk_instance_create( void )
{
    LOG_INFO( "instance_create (placeholder)" );



    return true;
}

static void
vk_instance_destroy( void )
{
    LOG_INFO( "instance_destroy (placeholder)" );

    /* TODO (Vulkan implementation):
       #if DEBUG
           vk_debug_messenger_destroy()
       #endif
       vkDestroyInstance( vk.instance, vk.alloc_cb )
       vk.instance = VK_NULL_HANDLE
    */
}

/*============================================================================================*/

static bool 
vk_instance_init()
{
    assert( vk.instance == VK_NULL_HANDLE );

    /* 1. Verify Vulkan 1.3 is available: */
   
    vk.version = vk_get_version();
    if ( vk.version < 3 || vk.version == 0 )
    {
        LOG_ERROR( "Vulkan 1.3 required; driver reports an older version" );
        return false;
    }

    /* get vulkan extentions and layers that will be specified during instance creation. */
    /* note: arrays size determinisitic, since we add < 8 extensions or layers */

    const char* exten_array[ 8 ] = { 0 };
    u32  exten_count = vk_instance_get_extensions( exten_array );
    if ( exten_count > 8 || exten_count == 0 )
    {
        LOG_ERROR( "vulkan extension buffer failure or overflow" );
        return false;
    }

    const char* layers_array[ 8 ] = { 0 };
    u32  layer_count = vk_instance_get_layers( layers_array );
    if ( layer_count > 8 )
    {
        LOG_ERROR( "vulkan layers buffer failure or overflow" );
        return false;
    }

   /*
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

   5. vkCreateInstance -> vk.instance

   6. Load instance-level function pointers:
          vk_lib_instance_entry_points()

   7. In debug builds, create the debug messenger:
          vk_debug_messenger_create()  (defined in vk_debug.c)
*/


    return true;
}


/*============================================================================================*/
// clang-format off