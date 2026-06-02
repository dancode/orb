/*==============================================================================================

    vulkan/vk_library.c -- Vulkan DLL loading and four-stage function pointer bootstrap.

    Vulkan is loaded at runtime so the engine has no static dependency on vulkan-1.dll.
    vk_functions.h is the single source of truth for every function pointer; it is included
    up to four times here, each time with a different VK_*_FUNCTION macro in effect:

      Stage 1 -- Exported:  GetProcAddress into vulkan-1.dll; yields vkGetInstanceProcAddr.
      Stage 2 -- Global:    vkGetInstanceProcAddr( NULL ) for pre-instance entry points.
      Stage 3 -- Instance:  vkGetInstanceProcAddr( instance ) after vkCreateInstance.
      Stage 4 -- Device:    vkGetDeviceProcAddr( device ) after vkCreateDevice.

    vk_lib_init() covers stages 1 and 2. Stages 3 and 4 must be called explicitly by the
    caller after creating the Vulkan instance and logical device respectively.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Declare all vulkan function pointers as file-scope statics
==============================================================================================*/

#define VK_EXPORTED_FUNCTION( fun )       static PFN_##fun fun;
#define VK_GLOBAL_LEVEL_FUNCTION( fun )   static PFN_##fun fun;
#define VK_INSTANCE_LEVEL_FUNCTION( fun ) static PFN_##fun fun;
#define VK_DEVICE_LEVEL_FUNCTION( fun )   static PFN_##fun fun;

#include "runtime_service/rhi/vk_functions.h"

/*============================================================================================*/

static bool
vk_lib_exported_entry_points()
{
    const char* func = NULL;

#define VK_EXPORTED_FUNCTION( fun )                                     \
    fun = ( PFN_##fun )sys_library_get_symbol( g_vk.dll, #fun );        \
    if ( !fun )                                                         \
    {                                                                   \
        func = #fun;                                                    \
        goto exit;                                                      \
    }

#include "runtime_service/rhi/vk_functions.h"

    return true;

exit:
    LOG_ERROR( "Could not load exported function: %s", func );
    return false;
}

/*============================================================================================*/

static bool
vk_lib_global_entry_points()
{
    const char* func = NULL;

#define VK_GLOBAL_LEVEL_FUNCTION( fun )                                 \
    fun = ( PFN_##fun )vkGetInstanceProcAddr( NULL, #fun );             \
    if ( !fun )                                                         \
    {                                                                   \
        func = #fun;                                                    \
        goto exit;                                                      \
    }

#include "runtime_service/rhi/vk_functions.h"

    return true;

exit:
    LOG_ERROR( "Could not load global level function: %s", func );
    return false;
}

/*============================================================================================*/

static bool
vk_lib_instance_entry_points()
{
    const char* func = NULL;

#define VK_INSTANCE_LEVEL_FUNCTION( fun )                               \
    fun = ( PFN_##fun )vkGetInstanceProcAddr( g_vk.instance, #fun );    \
    if ( !fun )                                                         \
    {                                                                   \
        func = #fun;                                                    \
        goto exit;                                                      \
    }

#include "runtime_service/rhi/vk_functions.h"

    return true;

exit:
    LOG_ERROR( "Could not load instance level function: %s", func );
    return false;
}

/*============================================================================================*/

static bool
vk_lib_device_entry_points()
{
    const char* func = NULL;

#define VK_DEVICE_LEVEL_FUNCTION( fun )                                 \
    fun = ( PFN_##fun )vkGetDeviceProcAddr( g_vk.device, #fun );        \
    if ( !fun )                                                         \
    {                                                                   \
        func = #fun;                                                    \
        goto exit;                                                      \
    }

#include "runtime_service/rhi/vk_functions.h"

    return true;

exit:
    LOG_ERROR( "Could not load device level function: %s", func );
    return false;
}

/*=============================================================================================
    Library loading -- stages 1 (exported) and 2 (global).
    Call vk_lib_instance_entry_points() and vk_lib_device_entry_points() separately after
    vkCreateInstance and vkCreateDevice succeed.
==============================================================================================*/

static bool
vk_lib_init()
{
    if ( g_vk.dll == NULL )
    {
#if OS_WINDOWS
        g_vk.dll = sys_library_load( "vulkan-1.dll" );
#elif OS_LINUX
        g_vk.dll = sys_library_load( "libvulkan.so.1" );
#elif OS_MAC
        /* MoltenVK ships as libMoltenVK.dylib or the Vulkan SDK loader */
        g_vk.dll = sys_library_load( "libvulkan.1.dylib" );
        if ( !g_vk.dll )
            g_vk.dll = sys_library_load( "libMoltenVK.dylib" );
#endif
    }

    if ( g_vk.dll == NULL )
    {
        LOG_ERROR( "could not load Vulkan library" );
        return false;
    }

    if ( !vk_lib_exported_entry_points() )
        return false;

    if ( !vk_lib_global_entry_points() )
        return false;

    return true;
}

/*==============================================================================================
    Library unloading
==============================================================================================*/

static void
vk_lib_exit()
{
    if ( g_vk.dll != NULL )
    {
        sys_library_unload( g_vk.dll );
        g_vk.dll = NULL;
    }
}

/*============================================================================================*/
// clang-format on