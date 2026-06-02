/*==============================================================================================

    vulkan/vk_library.c — Vulkan DLL loading and function pointer bootstrap.

==============================================================================================*/

/* forward declare vulkan function pointrs */

#define VK_EXPORTED_FUNCTION( fun )       static PFN_##fun fun;
#define VK_GLOBAL_LEVEL_FUNCTION( fun )   static PFN_##fun fun;
#define VK_INSTANCE_LEVEL_FUNCTION( fun ) static PFN_##fun fun;
#define VK_DEVICE_LEVEL_FUNCTION( fun )   static PFN_##fun fun;

#include "runtime_service/rhi/vk_functions.h"

/*============================================================================================*/

static bool
vk_lib_exported_entry_points()
{
    i8* func = NULL;

#define VK_EXPORTED_FUNCTION( fun )                            \
    fun = ( PFN_##fun )sys_library_get_symbol( vk.dll, #fun ); \
    if ( !fun )                                                \
    {                                                          \
        func = #fun;                                           \
        goto exit;                                             \
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
    i8* func = NULL;

#define VK_GLOBAL_LEVEL_FUNCTION( fun )                     \
    fun = ( PFN_##fun )vkGetInstanceProcAddr( NULL, #fun ); \
    if ( !fun )                                             \
    {                                                       \
        func = #fun;                                        \
        goto exit;                                          \
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
    i8* func = NULL;

#define VK_INSTANCE_LEVEL_FUNCTION( fun )                          \
    fun = ( PFN_##fun )vkGetInstanceProcAddr( vk.instance, #fun ); \
    if ( !fun )                                                    \
    {                                                              \
        func = #fun;                                               \
        goto exit;                                                 \
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
    i8* func = NULL;

#define VK_DEVICE_LEVEL_FUNCTION( fun )                        \
    fun = ( PFN_##fun )vkGetDeviceProcAddr( vk.device, #fun ); \
    if ( !fun )                                                \
    {                                                          \
        func = #fun;                                           \
        goto exit;                                             \
    }

#include "runtime_service/rhi/vk_functions.h"

    return true;

exit:
    LOG_ERROR( "Could not load device level function: %s", func );
    return false;
}

/*=============================================================================================
    Library loading
==============================================================================================*/

static bool
vk_lib_init()
{
#if !OS_WINDOWS
    return false;
#endif

    if ( g_vk.dll == NULL )
    {
        g_vk.dll = sys_library_load( "vulkan-1.dll" );
    }
    if ( g_vk.dll == NULL )
    {
        LOG_ERROR( "could not load vulkan-1.dll" );
        return false;
    }

    // vk_lib_exported_entry_points();
    // vk_lib_global_entry_points();

    return true; /* success */
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
