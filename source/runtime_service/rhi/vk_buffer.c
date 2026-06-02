/*==============================================================================================

    vulkan/vk_buffer.c -- VkBuffer lifecycle and slot management.

    Slot pool: g_vk.buffers[ VK_MAX_BUFFERS ] (vk_buffer_slot_t).
    Handle packing: see VK_MAKE_HANDLE / VK_HANDLE_IDX / VK_HANDLE_GEN in vk_state.c.

    CPU_TO_GPU and CPU_ONLY buffers are persistently mapped at creation time.
    GPU_ONLY buffers must be populated via vk_upload.c (staged copy).

==============================================================================================*/

/*==============================================================================================
    Slot allocation helpers
==============================================================================================*/

static i32
vk_buffer_alloc_slot( void )
{
    /* Linear scan for the first free slot (generation == 0). */
    for ( u32 i = 0; i < VK_MAX_BUFFERS; ++i )
    {
        if ( g_vk.buffers[ i ].generation == 0 )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_buffer_validate( rhi_buffer_t handle )
{
    u32 idx = VK_HANDLE_IDX( handle.id );
    u8  gen = VK_HANDLE_GEN( handle.id );
    return idx < VK_MAX_BUFFERS && gen != 0 && g_vk.buffers[ idx ].generation == gen;
}

/*==============================================================================================
    Buffer creation / destruction
==============================================================================================*/

static rhi_buffer_t
vk_buffer_create( const rhi_buffer_desc_t* desc )
{
    if ( !desc || desc->size == 0 )
        return ( rhi_buffer_t ){ RHI_NULL_HANDLE };

    i32 idx = vk_buffer_alloc_slot();
    if ( idx < 0 )
    {
        LOG_ERROR( "buffer pool exhausted (VK_MAX_BUFFERS = %d)", VK_MAX_BUFFERS );
        return ( rhi_buffer_t ){ RHI_NULL_HANDLE };
    }

    vk_buffer_slot_t* slot = &g_vk.buffers[ idx ];

    /* Increment generation; skip 0 so the handle is never null. */
    u8 gen = ( u8 )( slot->generation == 0 ? 1 : slot->generation );

    /* TODO (Vulkan implementation):
       VkBufferUsageFlags vk_usage = rhi_buffer_usage_to_vk( desc->usage );
       VkBufferCreateInfo ci = {
           .size        = desc->size,
           .usage       = vk_usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
       };
       vkCreateBuffer( g_vk.device, &ci, g_vk.alloc_cb, &slot->buffer )

       VkMemoryRequirements reqs;
       vkGetBufferMemoryRequirements2( g_vk.device, ... )

       vk_mem_alloc_t alloc;
       vk_mem_alloc( reqs, desc->memory, &alloc )
       vkBindBufferMemory( g_vk.device, slot->buffer, alloc.memory, alloc.offset )

       if ( desc->memory != RHI_MEMORY_GPU_ONLY )
           vkMapMemory( g_vk.device, alloc.memory, alloc.offset, desc->size, 0, &slot->mapped )

       if ( desc->debug_name )
           vk_debug_name_object( VK_OBJECT_TYPE_BUFFER, (u64)slot->buffer, desc->debug_name )
    */

    slot->size       = desc->size;
    slot->generation = gen;

    return ( rhi_buffer_t ){ VK_MAKE_HANDLE( gen, ( u32 )idx ) };
}

static void
vk_buffer_destroy( rhi_buffer_t handle )
{
    if ( !vk_buffer_validate( handle ) )
        return;

    u32               idx  = VK_HANDLE_IDX( handle.id );
    vk_buffer_slot_t* slot = &g_vk.buffers[ idx ];

    /* TODO:
       if ( slot->mapped )
           vkUnmapMemory( g_vk.device, ... )
       vkDestroyBuffer( g_vk.device, slot->buffer, g_vk.alloc_cb )
       vk_mem_free( ... )
    */

    /* Advance generation so stale handles fail validation. */
    slot->generation = ( u8 )( slot->generation + 1 );
    slot->buffer     = VK_NULL_HANDLE;
    slot->mapped     = NULL;
    slot->size       = 0;
}

static void
vk_buffer_write( rhi_buffer_t handle, const void* data, u32 size, u32 offset )
{
    if ( !vk_buffer_validate( handle ) || !data )
        return;

    vk_buffer_slot_t* slot = &g_vk.buffers[ VK_HANDLE_IDX( handle.id ) ];

    /* TODO: assert slot->mapped != NULL (caller must not write to GPU_ONLY buffers)
       memcpy( (u8*)slot->mapped + offset, data, size )
       -- No explicit flush needed if memory is HOST_COHERENT (our default for CPU_TO_GPU).
       -- For non-coherent memory: vkFlushMappedMemoryRanges( ... )
    */

    UNUSED( size );
    UNUSED( offset );
    UNUSED( slot );
}

/*============================================================================================*/
