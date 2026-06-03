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
    /* TODO:
       for ( u32 i = 0; i < vk.memory_props.memoryTypeCount; ++i )
           if ( (type_filter & (1u << i)) &&
                (vk.memory_props.memoryTypes[i].propertyFlags & required) == required )
               return i;
       return UINT32_MAX;   -- caller must handle failure
    */
    UNUSED( type_filter );
    UNUSED( required );
    return 0;
}

/*==============================================================================================
    Public allocation interface  (called by vk_buffer.c and vk_texture.c)
==============================================================================================*/

static bool
vk_mem_alloc( VkMemoryRequirements reqs, rhi_memory_t hint, vk_mem_alloc_t* out )
{
    UNUSED( reqs );
    UNUSED( hint );
    UNUSED( out );

    /* TODO:
       VkMemoryPropertyFlags flags = ...;   -- map rhi_memory_t -> VkMemoryPropertyFlags
       u32 type_idx = vk_memory_find_type( reqs.memoryTypeBits, flags );
       if ( type_idx == UINT32_MAX ) return false;

       VkMemoryAllocateInfo ai = {
           .allocationSize  = reqs.size,
           .memoryTypeIndex = type_idx,
       };
       vkAllocateMemory( vk.device, &ai, vk.alloc_cb, &out->memory )
       out->offset = 0;
    */

    return true;
}

static void
vk_mem_free( vk_mem_alloc_t alloc )
{
    UNUSED( alloc );
    /* TODO: vkFreeMemory( vk.device, alloc.memory, vk.alloc_cb ) */
}

/*============================================================================================*/
