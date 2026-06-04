/*==============================================================================================

    vulkan/vk_debug.c -- Validation messenger and GPU debug labels.

    All public functions in this file are guarded by #if DEBUG so they compile out
    cleanly in release builds.  Callers do not need to guard -- the API stubs in
    rhi_api.c delegate to these helpers which become empty bodies in release.

    Depends on VK_EXT_debug_utils being loaded in vk_functions.h.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Runtime severity filter.

    Mirrors the engine log_level_t scale.  Only Vulkan severity levels that map at or
    above this threshold are forwarded to the engine log.  Default is WARN so that
    errors and warnings are always visible without VERBOSE/INFO spam on startup.

    Call vk_debug_set_min_level() at any time -- no Vulkan objects are recreated.
==============================================================================================*/

static log_level_t g_vk_debug_min_level = LOG_LEVEL_ERROR;

static void
vk_debug_set_min_level( log_level_t level )
{
    g_vk_debug_min_level = level;
}

/*==============================================================================================
    Debug messenger callback  (validation layer output -> engine log)

    Vulkan severity   engine level    notes
    ---------------   ------------    -----
    VERBOSE           LOG_LEVEL_TRACE  per-call driver chatter; very spammy
    INFO              LOG_LEVEL_INFO   higher-level events; less frequent
    WARNING           LOG_LEVEL_WARN   spec violations that may work but shouldn't
    ERROR             LOG_LEVEL_ERROR  spec violations that will cause corruption or crashes

    No Vulkan equivalent exists for LOG_LEVEL_DEBUG or LOG_LEVEL_FATAL.
==============================================================================================*/

#if DEBUG
static log_level_t
vk_severity_to_log_level( VkDebugUtilsMessageSeverityFlagBitsEXT severity )
{
    switch ( severity )
    {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:   return LOG_LEVEL_ERROR;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: return LOG_LEVEL_WARN;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:    return LOG_LEVEL_INFO;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: return LOG_LEVEL_TRACE;
        default:                                              return LOG_LEVEL_ERROR;
    }
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
vk_debug_callback( VkDebugUtilsMessageSeverityFlagBitsEXT       severity,
                   VkDebugUtilsMessageTypeFlagsEXT              type,
                   const VkDebugUtilsMessengerCallbackDataEXT*  cb_data,
                   void*                                        user_data )
{
    UNUSED( type );
    UNUSED( user_data );

    log_level_t level = vk_severity_to_log_level( severity );
    if ( level < g_vk_debug_min_level )
        return VK_FALSE;

    core()->log_write( level, LOG_CH, "DBG: %s", cb_data->pMessage );

    return VK_FALSE; /* returning VK_TRUE would abort the Vulkan call that triggered this */
}
#endif /* DEBUG */

/*==============================================================================================
    Single source of truth for messenger create info settings.

    Used by vk_debug_messenger_create() and chained into VkInstanceCreateInfo.pNext
    by vk_instance_create() to capture messages during vkCreateInstance /
    vkDestroyInstance before the persistent messenger exists.
==============================================================================================*/

#if DEBUG
static void
vk_debug_messenger_fill_ci( VkDebugUtilsMessengerCreateInfoEXT* ci )
{
    ci->sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;

    ci->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT   |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT    |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;

    ci->messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT     |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT  |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    ci->pfnUserCallback = vk_debug_callback;
}
#endif /* DEBUG */

/*==============================================================================================
    Messenger lifecycle
==============================================================================================*/

static bool
vk_debug_messenger_create( void )
{
#if DEBUG

    VkDebugUtilsMessengerCreateInfoEXT ci = { 0 };
    vk_debug_messenger_fill_ci( &ci );

    VkResult r = vkCreateDebugUtilsMessengerEXT( vk.instance, &ci, vk.alloc_cb, &vk.debug_messenger );
    if ( r != VK_SUCCESS ) {
         LOG_ERROR( "vkCreateDebugUtilsMessengerEXT: %s", string_VkResult( r ) );
         return false;
    }
    LOG_INFO( "vkCreateDebugUtilsMessengerEXT: OK" );

#endif

    return true;
}

/*============================================================================================*/

static void
vk_debug_messenger_destroy( void )
{
#if DEBUG

    if ( vk.debug_messenger == VK_NULL_HANDLE )
         return;

    vkDestroyDebugUtilsMessengerEXT( vk.instance, vk.debug_messenger, vk.alloc_cb );
    vk.debug_messenger = VK_NULL_HANDLE;

#endif
}

/*==============================================================================================
    Object naming  (shows up in RenderDoc, Nsight, and validation messages)
==============================================================================================*/

static void
vk_debug_name_object( VkObjectType type, u64 handle, const char* name )
{
#if DEBUG

    if ( !vk.use_vk_ext_debug_utils || !vk.device || !name )
         return;

    VkDebugUtilsObjectNameInfoEXT info = { 0 };
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name;
    vkSetDebugUtilsObjectNameEXT( vk.device, &info );

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

    if ( !vk.use_vk_ext_debug_utils || !cmd || !name )
        return;

    VkDebugUtilsLabelEXT label = { 0 };
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[ 0 ] = r;
    label.color[ 1 ] = g;
    label.color[ 2 ] = b;
    label.color[ 3 ] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT( cmd->vk_cmd, &label );

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

    if ( !vk.use_vk_ext_debug_utils || !cmd )
        return;

    vkCmdEndDebugUtilsLabelEXT( cmd->vk_cmd );

#else

    UNUSED( cmd );

#endif
}

/*============================================================================================*/
// clang-format on
