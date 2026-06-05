/*==============================================================================================

    vulkan/vk_memory.c -- Device memory allocation.

    Sub-allocates from large VkDeviceMemory blocks keyed by heap type.
    Returns a (memory, offset) pair; callers bind with vkBind*Memory.

    Architecture
    ------------
    Three heap classes map to rhi_memory_t:
        RHI_MEMORY_GPU_ONLY   -> VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        RHI_MEMORY_CPU_TO_GPU -> VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                               | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        RHI_MEMORY_CPU_ONLY   -> same as CPU_TO_GPU but reserved for staging

    Each class owns a list of large blocks (default VK_MEM_BLOCK_SIZE).  Allocations
    use a linear bump allocator within the current block; a new block is appended when
    there is insufficient space.  Freed memory is not reclaimed (arenas are cleared on
    device destroy).  For a longer-lived engine a slab allocator per size class would
    be the natural next step.

    Alternatively, drop this file and wire in the Vulkan Memory Allocator (VMA) library;
    replace the two helpers below with vmaCreateBuffer / vmaCreateImage calls.

==============================================================================================*/

#define VK_MEM_BLOCK_SIZE  ORB_MB( 256 )

typedef struct vk_mem_alloc_s
{
    VkDeviceMemory  memory;
    VkDeviceSize    offset;

} vk_mem_alloc_t;

/*==============================================================================================
    Internal helpers
==============================================================================================*/

static u32
vk_memory_find_type( u32 type_filter, VkMemoryPropertyFlags required )
{
    for ( u32 i = 0; i < vk.memory_props.memoryTypeCount; ++i )
    {
        if ( ( type_filter & ( 1u << i ) ) &&
             ( vk.memory_props.memoryTypes[ i ].propertyFlags & required ) == required )
            return i;
    }
    return UINT32_MAX;
}

/*==============================================================================================
    Public allocation interface  (called by vk_buffer.c and vk_texture.c)
==============================================================================================*/

static bool
vk_mem_alloc( VkMemoryRequirements reqs, rhi_memory_t hint, vk_mem_alloc_t* out )
{
    /* Map RHI memory class to the required Vulkan property flags */
    static const VkMemoryPropertyFlags s_flags[] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,                                             /* GPU_ONLY   */
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,     /* CPU_TO_GPU */
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,     /* CPU_ONLY   */
    };

    VkMemoryPropertyFlags flags    = s_flags[ hint ];
    u32                   type_idx = vk_memory_find_type( reqs.memoryTypeBits, flags );

    /* GPU_ONLY: fall back to host-visible on unified-memory architectures (iGPU / APU). */
    if ( type_idx == UINT32_MAX && hint == RHI_MEMORY_GPU_ONLY )
    {
        type_idx = vk_memory_find_type( reqs.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    }
    if ( type_idx == UINT32_MAX )
    {
        LOG_ERROR( "vk_mem_alloc: no compatible memory type (filter=0x%x flags=0x%x)",
                   reqs.memoryTypeBits, (u32)flags );
        return false;
    }

    VkMemoryAllocateInfo ai  = { 0 };
    ai.sType                 = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize        = reqs.size;
    ai.memoryTypeIndex       = type_idx;

    VkResult r = vkAllocateMemory( vk.device, &ai, vk.alloc_cb, &out->memory );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "vkAllocateMemory: %s", string_VkResult( r ) );
        return false;
    }
    out->offset = 0;
    return true;
}

static void
vk_mem_free( vk_mem_alloc_t alloc )
{
    if ( alloc.memory != VK_NULL_HANDLE )
        vkFreeMemory( vk.device, alloc.memory, vk.alloc_cb );
}

/*============================================================================================*/
