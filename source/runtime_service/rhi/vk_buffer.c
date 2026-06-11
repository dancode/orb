/*==============================================================================================

    vulkan/vk_buffer.c -- VkBuffer lifecycle and slot management.

    Slot pool: vk.buffers[ VK_MAX_BUFFERS ] (vk_buffer_slot_t).
    handle.id is the slot index directly; 0 is unused (RHI_NULL_HANDLE).

    CPU_TO_GPU and CPU_ONLY buffers are persistently mapped at creation time.
    GPU_ONLY buffers must be populated via vk_upload.c (staged copy).

    Buffer usage flag conversion lives in vk_convert.c.

==============================================================================================*/

/*==============================================================================================
    Slot allocation helpers
==============================================================================================*/

static i32
vk_buffer_alloc_slot( void )
{
    for ( u32 i = 1; i < VK_MAX_BUFFERS; ++i )
    {
        if ( vk.buffers[ i ].buffer == VK_NULL_HANDLE )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_buffer_validate( rhi_buffer_t handle )
{
    return handle.id > 0 && handle.id < VK_MAX_BUFFERS
        && vk.buffers[ handle.id ].buffer != VK_NULL_HANDLE;
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

    vk_buffer_slot_t* slot = &vk.buffers[ (u32)idx ];

    VkBufferCreateInfo buf_ci = { 0 };
    buf_ci.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size               = desc->size;
    buf_ci.usage              = rhi_buffer_usage_to_vk( desc->usage );
    buf_ci.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer( vk.device, &buf_ci, vk.alloc_cb, &slot->buffer );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "buffer_create: vkCreateBuffer: %s", string_VkResult( r ) );
        return ( rhi_buffer_t ){ RHI_NULL_HANDLE };
    }

    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements( vk.device, slot->buffer, &reqs );

    VkMemoryAllocateFlags bda_flag = ( desc->usage & RHI_BUFFER_USAGE_DEVICE_ADDRESS )
                                   ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
    vk_mem_alloc_t alloc = { 0 };
    if ( !vk_mem_alloc( reqs, desc->memory, bda_flag, &alloc ) )
    {
        vkDestroyBuffer( vk.device, slot->buffer, vk.alloc_cb );
        slot->buffer = VK_NULL_HANDLE;
        return ( rhi_buffer_t ){ RHI_NULL_HANDLE };
    }
    slot->memory = alloc.memory;

    r = vkBindBufferMemory( vk.device, slot->buffer, slot->memory, alloc.offset );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "buffer_create: vkBindBufferMemory: %s", string_VkResult( r ) );
        vkFreeMemory   ( vk.device, slot->memory, vk.alloc_cb );
        vkDestroyBuffer( vk.device, slot->buffer, vk.alloc_cb );
        slot->memory = VK_NULL_HANDLE;
        slot->buffer = VK_NULL_HANDLE;
        return ( rhi_buffer_t ){ RHI_NULL_HANDLE };
    }

    /* Persistently map host-visible buffers; GPU-only buffers are populated by staged upload. */
    if ( desc->memory != RHI_MEMORY_GPU_ONLY )
    {
        r = vkMapMemory( vk.device, slot->memory, 0, desc->size, 0, &slot->mapped );
        if ( r != VK_SUCCESS )
        {
            LOG_ERROR( "buffer_create: vkMapMemory: %s", string_VkResult( r ) );
            vkFreeMemory   ( vk.device, slot->memory, vk.alloc_cb );
            vkDestroyBuffer( vk.device, slot->buffer, vk.alloc_cb );
            slot->memory = VK_NULL_HANDLE;
            slot->buffer = VK_NULL_HANDLE;
            return ( rhi_buffer_t ){ RHI_NULL_HANDLE };
        }
    }

    if ( desc->debug_name )
        vk_debug_name_object( VK_OBJECT_TYPE_BUFFER, (u64)slot->buffer, desc->debug_name );

    slot->size = desc->size;

    return ( rhi_buffer_t ){ (u32)idx };
}

static void
vk_buffer_destroy( rhi_buffer_t handle )
{
    if ( !vk_buffer_validate( handle ) )
        return;

    vk_buffer_slot_t* slot = &vk.buffers[ handle.id ];

    /* Deferred destroy: an in-flight frame may still read this buffer.  The unmap is deferred
       with it (entry carries the mapped pointer) so the memory stays valid meanwhile. */
    vk_garbage_push( &( vk_garbage_t ){
        .buffer = slot->buffer,
        .memory = slot->memory,
        .mapped = slot->mapped,
    } );

    slot->buffer = VK_NULL_HANDLE;
    slot->memory = VK_NULL_HANDLE;
    slot->mapped = NULL;
    slot->size   = 0;
}

static void
vk_buffer_write( rhi_buffer_t handle, const void* data, u32 size, u32 offset )
{
    if ( !vk_buffer_validate( handle ) || !data )
        return;

    vk_buffer_slot_t* slot = &vk.buffers[ handle.id ];

    /* Caller must not write to GPU_ONLY buffers; use upload_buffer for those. */
    if ( !slot->mapped )
    {
        LOG_ERROR( "buffer_write: buffer is GPU_ONLY; use upload_buffer for staged copies" );
        return;
    }
    if ( offset + size > slot->size )
    {
        LOG_ERROR( "buffer_write: out of bounds (offset=%u size=%u buffer_size=%u)", offset, size, slot->size );
        return;
    }

    /* Memory is HOST_COHERENT (our default for CPU-visible allocations); no flush needed. */
    memcpy( (u8*)slot->mapped + offset, data, size );
}

static u64
vk_buffer_get_device_address( rhi_buffer_t handle )
{
    if ( !vk_buffer_validate( handle ) ) return 0;
    VkBufferDeviceAddressInfo info = { 0 };
    info.sType                     = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer                    = vk.buffers[ handle.id ].buffer;
    return (u64)vkGetBufferDeviceAddress( vk.device, &info );
}

/*============================================================================================*/
