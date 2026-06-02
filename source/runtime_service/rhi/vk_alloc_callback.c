/*==============================================================================================

    vk_alloc_callback.c -- Optional VkAllocationCallbacks for host-side memory tracking.

    These callbacks intercept CPU-side allocations made by the Vulkan loader, validation
    layers, and some driver bookkeeping objects.  They do NOT intercept GPU memory
    (vkAllocateMemory) -- that is tracked separately in vk_memory.c.

    Practical value
    ---------------
    - See how much host RAM the validation layers consume (substantial during PSO creation).
    - pfnInternalAllocation fires when the driver compiles SPIR-V to native ISA.
      VK_INTERNAL_ALLOCATION_TYPE_EXECUTABLE means a pipeline-cache miss just caused a
      shader compilation stall -- invaluable for confirming the cache is working.
    - Route Vulkan host allocations through a custom allocator (future: tie into core
      arena once lifetime semantics are confirmed -- Vulkan holds some objects across
      the entire device lifetime).

    Disabled by default.  Set g_vk.use_vk_alloc_cb = true before rhi()->init() to enable.
    Callbacks must be thread-safe -- the driver can call them from any thread.

    Note: alloc callbacks fire BEFORE core is initialized (during vkCreateInstance), so
    output uses printf rather than LOG_*.  Replace when core is confirmed live first.

==============================================================================================*/

/* Platform headers for aligned allocation and size queries. */
#if OS_WINDOWS
    #include <malloc.h> /* _aligned_malloc, _aligned_realloc, _aligned_free */
#elif OS_LINUX
    #include <malloc.h> /* malloc_usable_size */
#elif OS_MAC
    #include <malloc/malloc.h> /* malloc_size */
#endif

/*==============================================================================================
    Platform-aware aligned allocation stubs.

    Vulkan may request alignments beyond malloc's default (up to 64 bytes for SIMD types).
    Replace with engine allocator when ready; aligned semantics must be preserved.
==============================================================================================*/

#if OS_WINDOWS

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
    /* aligned_alloc requires size to be a multiple of align. */
    size_t a  = align > 1 ? align : 1;
    size_t sz = ( size + a - 1 ) & ~( a - 1 );
    return aligned_alloc( a, sz );
}

static void*
vk_realloc_aligned( void* ptr, size_t size, size_t align )
{
    /* POSIX has no aligned_realloc.  Allocate new block, copy preserved bytes, free old. */
    void* new_ptr = vk_alloc_aligned( size, align );
    if ( new_ptr && ptr )
    {
        /* Query old size so we only copy valid bytes (avoids buffer overread). */
    #if OS_LINUX
        size_t old_size = malloc_usable_size( ptr );
    #elif OS_MAC
        size_t old_size = malloc_size( ptr );
    #else
        size_t old_size = size; /* conservative fallback -- may copy extra */
    #endif
        memcpy( new_ptr, ptr, old_size < size ? old_size : size );
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
    Tracking state (debug only)

    g_vk_alloc_host_total     : bytes currently live via pfnAllocation / pfnReallocation.
                                Realloc delta is approximate (we don't store old sizes).
    g_vk_alloc_internal_total : bytes the driver allocated internally (observe only).
==============================================================================================*/

static const bool cb_debug = DEBUG;

static i64 g_vk_alloc_host_total     = 0;
static i64 g_vk_alloc_internal_total = 0;

static const char*
vk_scope_name( VkSystemAllocationScope s )
{
    switch ( s )
    {
        case VK_SYSTEM_ALLOCATION_SCOPE_COMMAND: return "command";
        case VK_SYSTEM_ALLOCATION_SCOPE_OBJECT: return "object";
        case VK_SYSTEM_ALLOCATION_SCOPE_CACHE: return "cache";
        case VK_SYSTEM_ALLOCATION_SCOPE_DEVICE: return "device";
        case VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE: return "instance";
        default: return "?";
    }
}

static const char*
vk_internal_type_name( VkInternalAllocationType t )
{
    return t == VK_INTERNAL_ALLOCATION_TYPE_EXECUTABLE ? "executable" : "?";
}

/*==============================================================================================
    Callbacks
==============================================================================================*/

static void* VKAPI_CALL
vk_cb_alloc( void* userdata, size_t size, size_t align, VkSystemAllocationScope scope )
{
    UNUSED( userdata );

    void* ptr = vk_alloc_aligned( size, align );

    if ( cb_debug )
    {
        if ( ptr )
            g_vk_alloc_host_total += ( i64 )size;
        printf( "[vk:alloc] +%zu  align=%zu  scope=%-8s  total=%lld\n", size, align, vk_scope_name( scope ),
                ( long long )g_vk_alloc_host_total );
    }

    return ptr;
}

/*============================================================================================*/

static void* VKAPI_CALL
vk_cb_realloc( void* userdata, void* old_ptr, size_t size, size_t align, VkSystemAllocationScope scope )
{
    UNUSED( userdata );

    void* ptr = vk_realloc_aligned( old_ptr, size, align );

    if ( cb_debug )
    {
        /* Host total delta is approximate here -- we don't track individual alloc sizes. */
        printf( "[vk:realloc] %p -> %p  size=%zu  align=%zu  scope=%s\n", old_ptr, ptr, size, align,
                vk_scope_name( scope ) );
    }

    return ptr;
}

/*============================================================================================*/

static void VKAPI_CALL
vk_cb_free( void* userdata, void* ptr )
{
    UNUSED( userdata );

    if ( !ptr )
        return;

    vk_free_aligned( ptr );

    if ( cb_debug )
        printf( "[vk:free]  %p\n", ptr );
}

/*============================================================================================*/

static void VKAPI_CALL
vk_cb_internal_alloc( void* userdata, size_t size, VkInternalAllocationType type, VkSystemAllocationScope scope )
{
    UNUSED( userdata );

    if ( cb_debug )
    {
        g_vk_alloc_internal_total += ( i64 )size;
        /* VK_INTERNAL_ALLOCATION_TYPE_EXECUTABLE = driver just compiled SPIR-V to native ISA.
           If this fires during a frame, the pipeline cache miss caused a stall. */
        printf( "[vk:internal_alloc] +%zu  type=%-12s  scope=%-8s  internal_total=%lld\n", size,
                vk_internal_type_name( type ), vk_scope_name( scope ), ( long long )g_vk_alloc_internal_total );
    }
}

/*============================================================================================*/

static void VKAPI_CALL
vk_cb_internal_free( void* userdata, size_t size, VkInternalAllocationType type, VkSystemAllocationScope scope )
{
    UNUSED( userdata );

    if ( cb_debug )
    {
        g_vk_alloc_internal_total -= ( i64 )size;
        printf( "[vk:internal_free]  -%zu  type=%-12s  scope=%-8s  internal_total=%lld\n", size,
                vk_internal_type_name( type ), vk_scope_name( scope ), ( long long )g_vk_alloc_internal_total );
    }
}

/*==============================================================================================
    Static storage + init

    g_vk.alloc_cb is a pointer; this file owns the struct it points to.
    g_vk.alloc_cb stays NULL when use_vk_alloc_cb is false, so all Vulkan call sites
    pass NULL to use the driver's default allocator without any conditional logic.
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

    g_vk.alloc_cb                    = &s_alloc_cb;

    if ( cb_debug )
    {
        g_vk_alloc_host_total     = 0;
        g_vk_alloc_internal_total = 0;
    }
}

/*============================================================================================*/
