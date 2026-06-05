/*==============================================================================================

    vulkan/vk_memory.c -- Device memory allocation.

    Current implementation: one vkAllocateMemory call per resource.  offset is always 0.
    Returns a (memory, offset) pair; callers bind with vkBind*Memory.

    Memory classes
    --------------
    Three heap classes map to rhi_memory_t:
        RHI_MEMORY_GPU_ONLY   -> VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        RHI_MEMORY_CPU_TO_GPU -> VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                               | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        RHI_MEMORY_CPU_ONLY   -> same as CPU_TO_GPU but reserved for staging

    Known limit
    -----------
    Desktop GPUs expose maxMemoryAllocationCount ~4096.  At VK_MAX_TEXTURES=2048 plus
    VK_MAX_BUFFERS=1024 at full occupancy we approach that ceiling; allocation failures
    will occur once it is exceeded.

    Planned: replace with a slab/bump sub-allocator that issues one large VkDeviceMemory
    block per heap class and carves resources out of it -- matching the (memory, offset)
    interface callers already use.  Alternatively, wire in VMA (Vulkan Memory Allocator)
    and replace the two helpers below with vmaCreateBuffer / vmaCreateImage calls.

==============================================================================================*/

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
vk_mem_alloc( VkMemoryRequirements reqs, rhi_memory_t hint, VkMemoryAllocateFlags extra_flags,
              vk_mem_alloc_t* out )
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
        if ( type_idx != UINT32_MAX )
            LOG_WARN( "vk_mem_alloc: GPU_ONLY unavailable; falling back to host-visible (unified memory?)" );
    }
    if ( type_idx == UINT32_MAX )
    {
        LOG_ERROR( "vk_mem_alloc: no compatible memory type (filter=0x%x flags=0x%x)",
                   reqs.memoryTypeBits, (u32)flags );
        return false;
    }

    VkMemoryAllocateFlagsInfo fi = { 0 };
    fi.sType                     = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    fi.flags                     = extra_flags;

    VkMemoryAllocateInfo ai  = { 0 };
    ai.sType                 = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext                 = extra_flags ? &fi : NULL;
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
