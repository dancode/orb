/*==============================================================================================

    vk_alloc_callback.c -- Optional VkAllocationCallbacks for Vulkan host-side events.

    The main signal here is pfnInternalAllocation: it fires when the driver compiles
    SPIR-V to native ISA.  VK_INTERNAL_ALLOCATION_TYPE_EXECUTABLE means a pipeline-cache
    miss caused a shader compilation stall on this frame -- use this to confirm the
    pipeline cache is warming correctly.

    pfnAllocation / pfnReallocation / pfnFree are thin platform wrappers required by the
    spec whenever you provide a non-NULL VkAllocationCallbacks.  Route them through the
    engine allocator here once arena lifetime semantics are confirmed (Vulkan holds some
    objects for the full device lifetime).

    Disabled by default.  Set vk.use_vk_alloc_cb = true before rhi()->init() to enable.
    Callbacks must be thread-safe -- the driver may call them from any thread.

==============================================================================================*/

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
    // Note: You can't safely implement it on POSIX without size tracking. 
    // Worth a comment at minimum, or switch to alloc + free until you 

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
    Callbacks
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
    vk.alloc_cb stays NULL when use_vk_alloc_cb is false, so Vulkan call sites
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

/*============================================================================================*/
