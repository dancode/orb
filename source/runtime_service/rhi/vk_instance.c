/*==============================================================================================

    vulkan/vk_instance.c -- VkInstance + debug messenger.

    First step in global init.  Owns the connection to the Vulkan loader and
    the validation diagnostic plumbing in debug builds.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Vulkan extensions provide additional functionality beyond the core Vulkan API .
    We use this function to acquire our required vulkan extentions.

    VK_KHR_WIN32_SURFACE_EXTENSION_NAME: required for creating surfaces from Win32 HWNDs.
    VK_KHR_SURFACE_EXTENSION_NAME : required for surface support on all platforms.
==============================================================================================*/

static i32
vk_instance_get_extensions( const char** out_ext_array )
{
    i32                   out_ext_count   = 0;
    u32                   ext_count       = 0;
    VkExtensionProperties ext_props[ 64 ] = { 0 };

    /* collect list of vulkan extentions */

    VkResult r = vkEnumerateInstanceExtensionProperties( NULL, &ext_count, NULL );    
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "vkEnumerateInstanceExtensionProperties: %s", string_VkResult( r ) );
         return 0;
    }
    if ( ext_count > 64 ) {
         LOG_WARN( "instance extension count %u exceeds buffer (64); clamping", ext_count );
         ext_count = 64;
    }
    vkEnumerateInstanceExtensionProperties( NULL, &ext_count, ext_props );    

    /* validate the extentions we require exists. */
    
    bool has_debug_utils_ext = false;
    for ( u32 i = 0; i < ext_count; i++ )
    {
        const char* ext_str = ext_props[ i ].extensionName;
        if ( strcmp( VK_KHR_WIN32_SURFACE_EXTENSION_NAME, ext_str ) == 0 ) {
             vk.ext_win32_surface = true;
        }
        if ( strcmp( VK_KHR_SURFACE_EXTENSION_NAME, ext_str ) == 0 ) {
             vk.ext_khr_surface = true;
        }
        if ( strcmp( VK_EXT_DEBUG_UTILS_EXTENSION_NAME, ext_str ) == 0 ){
             has_debug_utils_ext = true;
        }
    }
    
    #if OS_WINDOWS
    if ( vk.ext_win32_surface == false || vk.ext_khr_surface == false ) {
         LOG_FATAL( "failed to get win32 surface extensions" );
    }
    #endif
    
    /* add the extentions to extention list */
    
    out_ext_array[ 0 ] = VK_KHR_SURFACE_EXTENSION_NAME;
    out_ext_count = 1;
    #if OS_WINDOWS
        out_ext_array[ out_ext_count++ ] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    #endif
    
    /*  warn if requested layers are not available */
    
    if ( vk.use_vk_ext_debug_utils && has_debug_utils_ext == false ) {
         LOG_WARN( "debug utils extension requested but not available; debug messenger disabled" );
         vk.use_vk_ext_debug_utils = false;
    }
    if ( vk.use_vk_ext_debug_utils ) {
         out_ext_array[ out_ext_count++ ] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    
    /* debug list our available extensions */

    LOG_INFO( "%u extensions supported", ext_count );
    for ( u32 i = 0; i < ext_count; i++ ) {
        LOG_TRACE( "EXT: %s", ext_props[ i ].extensionName );
    }

    return out_ext_count;
}

/*==============================================================================================
    Vulkan layers are optional components that can provide additional functionality or
    validation for Vulkan applications. We use this function to acquire our layers.

    VK_LAYER_KHRONOS_validation : Perform error checking and validation of Vulkan API usage.
                                  Should be disabled in release builds for performance.
    VK_LAYER_LUNARG_monitor     : A lightweight performance tracker build in.
                                  Adds (FPS) and Frame Time (in window title or terminal)
==============================================================================================*/

static i32 /* count */
vk_instance_get_layers( const char** out_layer_names )
{
    i32 out_layer_count = 0;

    /* skip api calls if we aren't using any layers */

    if ( !vk.use_vk_layer_validation && 
         !vk.use_vk_layer_monitor ) return 0;

    /* get the instance support layers list. */
    
    u32               layer_count = 0;
    VkLayerProperties layer_props[ 32 ] = { 0 };

    /* get available vulkan layers */
    
    VkResult r = vkEnumerateInstanceLayerProperties( &layer_count, NULL );
    if ( r != VK_SUCCESS ) {
         LOG_FATAL( "vkEnumerateInstanceLayerProperties: %s", string_VkResult( r ) );
    }
    assert( layer_count <= 32 );
    vkEnumerateInstanceLayerProperties( &layer_count, layer_props );    
    
    /* set all the instance layers we require */

    static const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    static const char* monitor_layer = "VK_LAYER_LUNARG_monitor";
    
    bool has_layer_validation = false;
    bool has_layer_monitor    = false;
    
    /* validate debug layers we require exist in layer list. */
    
    for ( u32 i = 0; i < layer_count; i++ )
    {   
        if ( strcmp( validation_layer, layer_props[ i ].layerName ) == 0 )
             has_layer_validation = true;

        if ( strcmp( monitor_layer, layer_props[ i ].layerName ) == 0 )
             has_layer_monitor = true;
    }
    
    /*  warn if requested layers are not available */

    if ( vk.use_vk_layer_validation && has_layer_validation == false ) {
        LOG_WARN( "validation layer requested but not available; validation disabled" );
        vk.use_vk_layer_validation = false;
    }
    if ( vk.use_vk_layer_monitor && has_layer_monitor == false ) {
        LOG_WARN( "monitor layer requested but not available; monitor disabled" );
        vk.use_vk_layer_monitor = false;
    }

    /*  only enable if we are using validation and monitoring */

    if ( vk.use_vk_layer_validation ) {
         out_layer_names[ out_layer_count ] = validation_layer;
         out_layer_count++;
    }
    if ( vk.use_vk_layer_monitor  ) {
         out_layer_names[ out_layer_count ] = monitor_layer;
         out_layer_count++;
    }

    /* debug list our available layers */

    LOG_INFO( "%u layers supported", layer_count );
    for ( u32 i = 0; i < layer_count; i++ ) {
         LOG_TRACE( "LYR: %s", layer_props[ i ].layerName );    
    }

    return out_layer_count;
}

/*==============================================================================================
    Acquire our version of Vulkan supported by the driver. 
==============================================================================================*/

static u32
vk_get_version( void )
{
    u32 api_version = 0;
    VkResult r = vkEnumerateInstanceVersion( &api_version );
    if ( r != VK_SUCCESS )
        LOG_FATAL( "failed to query vulkan api version" );

    LOG_INFO( "vulkan api version: %d.%d.%d",
              VK_VERSION_MAJOR( api_version ),
              VK_VERSION_MINOR( api_version ),
              VK_VERSION_PATCH( api_version ) );
    return api_version;
}

static const char*
vk_version_string( u32 version )
{
    static char buf[ 32 ] = {0};
    snprintf( buf, 32, "%d.%d.%d", VK_VERSION_MAJOR( version ), 
                                   VK_VERSION_MINOR( version ),
                                   VK_VERSION_PATCH( version ));
    return buf;
}

/*==============================================================================================
    Call Vulkan loader to create a Vulkan instance with the specified extension + layers.
==============================================================================================*/

static bool
vk_instance_create( u32 layer_count, const char** layer_array, u32 ext_count, const char** ext_array )
{
    /* Describe the application and the minimum Vulkan API version required. */

    VkApplicationInfo app_info  = { 0 };
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "orb";
    app_info.applicationVersion = VK_MAKE_API_VERSION( 0, 1, 0, 0 );
    app_info.pEngineName        = "orb";
    app_info.engineVersion      = VK_MAKE_API_VERSION( 0, ORB_VERSION_MAJOR, ORB_VERSION_MINOR, ORB_VERSION_PATCH );
    app_info.apiVersion         = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci    = { 0 };
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app_info;
    ci.enabledExtensionCount   = ext_count;
    ci.ppEnabledExtensionNames = ext_array;
    ci.enabledLayerCount       = layer_count;
    ci.ppEnabledLayerNames     = layer_array;

    /* Chain a debug messenger CI into pNext so validation messages emitted during
       vkCreateInstance / vkDestroyInstance can trigger validation (temporary).
       The real persistent messenger is created via vk_debug_messenger_create() */

    #if DEBUG
        VkDebugUtilsMessengerCreateInfoEXT dbg_ci = { 0 };
        if ( vk.use_vk_ext_debug_utils ) {
             vk_debug_messenger_fill_ci( &dbg_ci );
             ci.pNext = &dbg_ci;
        }
    #endif
    
    VkResult r = vkCreateInstance( &ci, vk.alloc_cb, &vk.instance );
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "vkCreateInstance: %s", string_VkResult( r ) );
         return false;
    }

    LOG_INFO( "vk_instance_create: OK" );
    return true;
}

/*============================================================================================*/
/* Destroy the Vulkan instance, which will also destroy the debug messenger if it exists. */

static void
vk_instance_destroy( void )
{
    if ( vk.instance == VK_NULL_HANDLE )
        return;

    vk_debug_messenger_destroy();
    vkDestroyInstance( vk.instance, vk.alloc_cb );
    vk.instance = VK_NULL_HANDLE;

    LOG_INFO( "instance_destroy: OK" );
}

/*==============================================================================================
    Initialize the Vulkan instance, which represents our connection to the Vulkan loader
    and provides access to global Vulkan functionality.  Also creates a debug messenger if
    VK_EXT_debug_utils is available and enabled. 

    We require Vulkan 1.3 for bindless descriptors and other features.
==============================================================================================*/   

static bool 
vk_instance_init()
{
    assert( vk.instance == VK_NULL_HANDLE );

    /* --- Verify Vulkan 1.3 is available: --- */
    
    vk.version = vk_get_version();
    if ( vk.version < VK_API_VERSION_1_3 )
    {
        LOG_ERROR( "Vulkan 1.3 required; driver reports %d.%d",
                   VK_VERSION_MAJOR( vk.version ), VK_VERSION_MINOR( vk.version ) );
        return false;
    }

    /* --- Get Vulkan extensions and layers we will specify during instance creation: --- */

    const char* exten_array[ 8 ] = { 0 };
    i32  exten_count = vk_instance_get_extensions( exten_array );
    if ( exten_count >= 8 || exten_count == 0 ) {
         LOG_ERROR( "vulkan extension buffer failure or overflow" );
         return false;
    }

    const char* layers_array[ 8 ] = { 0 };
    i32  layer_count = vk_instance_get_layers( layers_array );
    if ( layer_count >= 8 ) {
         LOG_ERROR( "vulkan layers buffer overflow" );
         return false;
    }

    /* --- Create instance --- */

    if ( !vk_instance_create( layer_count, layers_array, exten_count, exten_array )) {
         return false;
    }

    /* --- Load instance-level function pointers now that vk.instance is live --- */

    if ( !vk_lib_instance_entry_points() ){
          vk_instance_destroy();
          return false;
    }

    /* --- Create the persistent debug messenger (requires instance-level functions) --- */

    if ( vk.use_vk_ext_debug_utils ) {
         vk_debug_messenger_create();         
    }

    return true;
}


/*============================================================================================*/
// clang-format off