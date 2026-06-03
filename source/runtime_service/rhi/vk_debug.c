/*==============================================================================================

    vulkan/vk_debug.c -- Validation messenger and GPU debug labels.

    All public functions in this file are guarded by #if DEBUG so they compile out
    cleanly in release builds.  Callers do not need to guard -- the API stubs in
    rhi_api.c delegate to these helpers which become empty bodies in release.

    Depends on VK_EXT_debug_utils being loaded in vk_functions.h.

==============================================================================================*/
// clang-format off
/*==============================================================================================
    Debug messenger  (validation layer output -> engine log)
==============================================================================================*/

#if DEBUG

static VKAPI_ATTR VkBool32 VKAPI_CALL
vk_debug_callback( VkDebugUtilsMessageSeverityFlagBitsEXT       severity,
                   VkDebugUtilsMessageTypeFlagsEXT              type,
                   const VkDebugUtilsMessengerCallbackDataEXT*  cb_data,
                   void*                                        user_data )
{
    UNUSED( type );
    UNUSED( user_data );

    switch ( severity )
    {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT: {
            LOG_ERROR( "%s", cb_data->pMessage );
            break;
        }
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: {
            LOG_WARN( "%s", cb_data->pMessage );
            break;
        } 
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT: {
            LOG_INFO( "%s", cb_data->pMessage );
            break;
        }
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: {
            LOG_TRACE( "%s", cb_data->pMessage );
            break;
        }
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT: {
            break; /* stop warning */
        }
    }

    return VK_FALSE; /* returning VK_TRUE would abort the Vulkan call that triggered this */
}

#endif /* DEBUG */

/*============================================================================================*/

static bool
vk_debug_messenger_create( void )
{
#if DEBUG

    VkDebugUtilsMessengerCreateInfoEXT ci = { 0 };

    ci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = 0;
    ci.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    ci.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    ci.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;    // very verbose.
    ci.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;

    ci.messageType = 0;
    ci.messageType |= VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    ci.messageType |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    ci.messageType |= VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    ci.pfnUserCallback = vk_debug_callback;

    vkCreateDebugUtilsMessengerEXT( vk.instance, &ci, vk.alloc_cb, &vk.debug_messenger );
    
    printf( "debug_messenger_create\n" );

#endif
    return true;
}

/*============================================================================================*/

static void
vk_debug_messenger_destroy( void )
{
#if DEBUG
    /* TODO: vkDestroyDebugUtilsMessengerEXT( vk.instance, vk.debug_messenger, vk.alloc_cb )
    */
    printf( "[rhi:vk] debug_messenger_destroy (placeholder)\n" );
#endif
}

/*==============================================================================================
    Object naming  (shows up in RenderDoc, Nsight, and validation messages)
==============================================================================================*/

static void
vk_debug_name_object( VkObjectType type, u64 handle, const char* name )
{
#if DEBUG
    if ( !name || !vk.device )
        return;

    /* TODO:
       VkDebugUtilsObjectNameInfoEXT info = {
           .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
           .objectType   = type,
           .objectHandle = handle,
           .pObjectName  = name,
       };
       vkSetDebugUtilsObjectNameEXT( vk.device, &info )
    */
    UNUSED( type );
    UNUSED( handle );
#else
    UNUSED( type );
    UNUSED( handle );
    UNUSED( name );
#endif
}

/*==============================================================================================
    GPU label regions  (visible in GPU captures)
==============================================================================================*/

static void
vk_cmd_begin_label( rhi_command_list_t cmd, const char* name, f32 r, f32 g, f32 b )
{
#if DEBUG
    if ( !cmd || !name )
        return;
    /* TODO:
       VkDebugUtilsLabelEXT label = {
           .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
           .pLabelName = name,
           .color      = { r, g, b, 1.0f },
       };
       vkCmdBeginDebugUtilsLabelEXT( cmd->vk_cmd, &label )
    */
    UNUSED( r );
    UNUSED( g );
    UNUSED( b );
#else
    UNUSED( cmd );
    UNUSED( name );
    UNUSED( r );
    UNUSED( g );
    UNUSED( b );
#endif
}

/*============================================================================================*/

static void
vk_cmd_end_label( rhi_command_list_t cmd )
{
#if DEBUG
    if ( !cmd )
        return;
    /* TODO: vkCmdEndDebugUtilsLabelEXT( cmd->vk_cmd ) */
#else
    UNUSED( cmd );
#endif
}

/*============================================================================================*/
// clang-format on