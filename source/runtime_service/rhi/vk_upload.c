/*==============================================================================================

    vulkan/vk_upload.c -- Staged upload ring buffer for GPU-only resources.

    One staging buffer per frame-in-flight slot (VK_STAGING_SIZE bytes each).
    The slot indexed by (vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT) is the active one.
    frame_begin resets the head after waiting for the fence (GPU done with that slot).

    Upload flow:
        1. Caller calls rhi()->upload_buffer / upload_texture.
        2. Data is copied into the staging ring at the current head.
        3. A VkCopyBufferCmd or VkCopyBufferToImageCmd is recorded into a dedicated
           transfer command buffer for this slot.
        4. At frame_begin for the NEXT use of this slot, the fence has already been
           waited on, so the ring is safe to reset.

    For buffers larger than VK_STAGING_SIZE the caller must split the upload.
    TODO: grow by overflow allocation or add a large-object path.

==============================================================================================*/

static bool
vk_upload_init( void )
{
    printf( "[rhi:vk] upload_init (placeholder)\n" );

    /* TODO: for each slot in [0..VK_MAX_FRAMES_IN_FLIGHT):
       VkBufferCreateInfo ci = {
           .size        = VK_STAGING_SIZE,
           .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
       };
       vkCreateBuffer -> vk.staging[i].buffer
       vkAllocateMemory (HOST_VISIBLE | HOST_COHERENT) -> vk.staging[i].memory
       vkBindBufferMemory
       vkMapMemory -> vk.staging[i].mapped   (persists until shutdown)
       vk.staging[i].head = 0;
    */

    return true;
}

static void
vk_upload_shutdown( void )
{
    /* TODO: for each slot:
       vkUnmapMemory, vkDestroyBuffer, vkFreeMemory
    */
}

/*==============================================================================================
    Alloc from the active staging slot
==============================================================================================*/

typedef struct vk_staging_alloc_s
{
    void*         cpu_ptr;    /* write destination on the host */
    VkBuffer      buffer;     /* staging VkBuffer to use as transfer source */
    VkDeviceSize  offset;     /* byte offset within buffer */

} vk_staging_alloc_t;

static bool
vk_staging_alloc( u32 size, u32 align, vk_staging_alloc_t* out )
{
    u32 slot = vk.global_frame % VK_MAX_FRAMES_IN_FLIGHT;
    vk_staging_t* s = &vk.staging[ slot ];

    /* TODO:
       u32 aligned_head = ALIGN_UP( s->head, align );
       if ( aligned_head + size > VK_STAGING_SIZE ) return false;
       out->cpu_ptr = (u8*)s->mapped + aligned_head;
       out->buffer  = s->buffer;
       out->offset  = aligned_head;
       s->head      = aligned_head + size;
    */

    UNUSED( size );
    UNUSED( align );
    UNUSED( out );
    UNUSED( s );
    return false;   /* placeholder */
}

/*==============================================================================================
    Upload helpers  (called via rhi_api.c wiring)
==============================================================================================*/

static bool
vk_upload_buffer( rhi_buffer_t dst, const void* data, u32 size )
{
    UNUSED( dst );
    UNUSED( data );
    UNUSED( size );

    /* TODO:
       vk_staging_alloc_t sa;
       if ( !vk_staging_alloc( size, 4, &sa ) ) return false;
       memcpy( sa.cpu_ptr, data, size );

       VkBufferCopy region = { .srcOffset = sa.offset, .size = size };
       VkBuffer dst_buf = vk.buffers[ VK_HANDLE_IDX(dst.id) ].buffer;
       vkCmdCopyBuffer( transfer_cmd, sa.buffer, dst_buf, 1, &region )
       -- transfer_cmd is the active transfer command buffer for this frame slot.
       -- The copy executes before the graphics queue sees the buffer (pipeline barrier needed).
    */

    return false;
}

static bool
vk_upload_texture( rhi_texture_t dst, const void* data, u32 data_size, u16 mip, u16 layer )
{
    UNUSED( dst );
    UNUSED( data );
    UNUSED( data_size );
    UNUSED( mip );
    UNUSED( layer );

    /* TODO:
       vk_staging_alloc_t sa;
       if ( !vk_staging_alloc( data_size, 16, &sa ) ) return false;
       memcpy( sa.cpu_ptr, data, data_size );

       Transition image to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL (sync2 barrier).

       VkBufferImageCopy region = {
           .bufferOffset      = sa.offset,
           .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
           .imageExtent       = { width >> mip, height >> mip, 1 },
       };
       vkCmdCopyBufferToImage( transfer_cmd, sa.buffer, dst_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region )

       Transition to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for sampled use.
    */

    return false;
}

/*==============================================================================================
    Flush  (called from vk_frame.c at the top of frame_begin after fence wait)
==============================================================================================*/

static void
vk_upload_flush( u32 slot )
{
    UNUSED( slot );

    /* TODO:
       Submit the transfer command buffer for this slot on vk.transfer_queue.
       Use a timeline semaphore so the graphics queue waits before reading
       any buffers or textures that were written this slot.
       Reset the staging head:  vk.staging[slot].head = 0;
    */
}

/*============================================================================================*/
