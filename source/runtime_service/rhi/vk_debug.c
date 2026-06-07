/*==============================================================================================

    vk_debug.c -- Debug infrastructure: VkAllocationCallbacks, validation messenger,
                  object naming, and GPU command labels.

    Allocation callbacks:
        pfnInternalAllocation fires when the driver compiles SPIR-V to native ISA.
        VK_INTERNAL_ALLOCATION_TYPE_EXECUTABLE means a pipeline-cache miss caused a
        shader compilation stall on this frame -- use this to confirm the cache is
        warming correctly.  Disabled by default; set vk.use_vk_alloc_cb = true
        before rhi()->init() to enable.  Callbacks must be thread-safe.

    Messenger, object naming, and GPU labels:
        All guarded by #if DEBUG -- compile out cleanly in release.  Callers do not
        need their own guards; the stubs produce empty bodies.

    Depends on VK_EXT_debug_utils being loaded in vk_functions.h.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Platform aligned allocation -- required by the spec; no tracking.
==============================================================================================*/

#if OS_WINDOWS
    #include <malloc.h> /* _aligned_malloc, _aligned_realloc, _aligned_free */

static void*
vk_alloc_aligned( size_t size, size_t align )
{
    return _aligned_malloc( size, align );
}

static void*
vk_realloc_aligned( void* ptr, size_t size, size_t align )
{
    return _aligned_realloc( ptr, size, align );
}

static void
vk_free_aligned( void* ptr )
{
    _aligned_free( ptr );
}

#else /* POSIX */

static void*
vk_alloc_aligned( size_t size, size_t align )
{
    void* ptr = NULL;
    posix_memalign( &ptr, align > sizeof( void* ) ? align : sizeof( void* ), size );
    return ptr;
}

static void*
vk_realloc_aligned( void* ptr, size_t size, size_t align )
{
    /* POSIX has no aligned_realloc: allocate + copy + free the old block.
       This loses the original size, so it over-copies if new size < old size --
       safe for Vulkan host allocations which only grow. */
    void* new_ptr = vk_alloc_aligned( size, align );
    if ( new_ptr && ptr )
    {
        memcpy( new_ptr, ptr, size );
        free( ptr );
    }
    return new_ptr;
}

static void
vk_free_aligned( void* ptr )
{
    free( ptr );
}

#endif /* OS_WINDOWS */

/*==============================================================================================
    Allocation callbacks
==============================================================================================*/

static void* VKAPI_CALL
vk_cb_alloc( void* userdata, size_t size, size_t align, VkSystemAllocationScope scope )
{
    UNUSED( userdata );
    UNUSED( scope );
    return vk_alloc_aligned( size, align );
}

/*============================================================================================*/

static void* VKAPI_CALL
vk_cb_realloc( void* userdata, void* old_ptr, size_t size, size_t align, VkSystemAllocationScope scope )
{
    UNUSED( userdata );
    UNUSED( scope );
    return vk_realloc_aligned( old_ptr, size, align );
}

/*============================================================================================*/

static void VKAPI_CALL
vk_cb_free( void* userdata, void* ptr )
{
    UNUSED( userdata );
    vk_free_aligned( ptr );
}

/*============================================================================================*/

static void VKAPI_CALL
vk_cb_internal_alloc( void* userdata, size_t size, VkInternalAllocationType type, VkSystemAllocationScope scope )
{
    UNUSED( userdata );
    UNUSED( scope );

    /* EXECUTABLE = driver just compiled SPIR-V to native ISA on this thread.
       If this fires mid-frame, a pipeline-cache miss caused a compilation stall. */
    if ( type == VK_INTERNAL_ALLOCATION_TYPE_EXECUTABLE )
        printf( "[vk] shader compilation: %zu bytes\n", size );
}

/*============================================================================================*/

static void VKAPI_CALL
vk_cb_internal_free( void* userdata, size_t size, VkInternalAllocationType type, VkSystemAllocationScope scope )
{
    UNUSED( userdata );
    UNUSED( size );
    UNUSED( type );
    UNUSED( scope );
}

/*==============================================================================================
    Static storage + init

    vk.alloc_cb is a pointer; this file owns the struct it points to.
    vk.alloc_cb stays NULL when use_vk_alloc_cb is false, so all Vulkan call sites
    pass NULL transparently to use the driver's default allocator.
==============================================================================================*/

static VkAllocationCallbacks s_alloc_cb;

static void
vk_allocation_callback_init( void )
{
    s_alloc_cb.pUserData             = NULL;
    s_alloc_cb.pfnAllocation         = vk_cb_alloc;
    s_alloc_cb.pfnReallocation       = vk_cb_realloc;
    s_alloc_cb.pfnFree               = vk_cb_free;
    s_alloc_cb.pfnInternalAllocation = vk_cb_internal_alloc;
    s_alloc_cb.pfnInternalFree       = vk_cb_internal_free;

    vk.alloc_cb = &s_alloc_cb;
}

/*==============================================================================================
    Runtime severity filter.

    Mirrors the engine log_level_t scale.  Only Vulkan severity levels that map at or
    above this threshold are forwarded to the engine log.  Default is ERROR so that
    severe messages are always visible without VERBOSE/INFO spam on startup.

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
    LOG_INFO( "vk_debug_messenger_create: OK" );

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

    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !vk.use_vk_ext_debug_utils || !cl || !name )
        return;

    VkDebugUtilsLabelEXT label = { 0 };
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[ 0 ] = r;
    label.color[ 1 ] = g;
    label.color[ 2 ] = b;
    label.color[ 3 ] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT( cl->vk_cmd, &label );

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

    struct rhi_command_list_s* cl = vk_cmd_from_handle( cmd );
    if ( !vk.use_vk_ext_debug_utils || !cl )
        return;

    vkCmdEndDebugUtilsLabelEXT( cl->vk_cmd );

#else

    UNUSED( cmd );

#endif
}

/*============================================================================================*/
// clang-format on
