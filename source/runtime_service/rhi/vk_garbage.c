/*==============================================================================================

    vk_garbage.c -- deferred destruction of backing GPU objects.

    Destroying a VkImage / VkBuffer / VkSampler the instant a caller asks is unsafe: the object
    may still be referenced by command buffers from frames that are in flight.  Freeing it then
    produces a GPU page fault -> VK_ERROR_DEVICE_LOST on the next fence wait.  (This is exactly
    what a runtime font swap hit: tt_font_unload destroyed an atlas image still bound by the
    previous frames' bindless set.)

    The bindless layer already defers descriptor-slot reuse on an epoch timeline (see
    vk_descriptor.c).  This file extends the same idea to the backing objects themselves:
    vk_*_destroy() hands its Vk handles here instead of calling vkDestroy* directly, tagged with
    a safe_at epoch (global_epoch + VK_MAX_FRAMES_IN_FLIGHT).  vk_garbage_collect(), called from
    vk_frame_begin right after the descriptor flush, frees entries once global_epoch has caught
    up -- by which point every context has completed a full fence cycle and no in-flight graphics
    command buffer can still reference them.

    A resource can also be referenced by the transfer queue: anything uploaded via the staging
    path (vk_upload.c) is copied by a transfer command buffer that is submitted at the next flush
    and tracked by upload_timeline, a clock independent of global_epoch.  Each entry therefore
    also carries an upload_wait timeline value and is freed only once that value has been reached.
    Without this second gate, freeing a resource the same frame it was uploaded (before the DMA
    copy has run) destroys a VkImage/VkBuffer still in use by the transfer command buffer.

    One entry holds all of a resource's backing objects (image + view + memory, or buffer +
    memory, or sampler), so queue depth is one slot per destroyed resource, not per Vk object.
    safe_at is always global_epoch + a constant, and global_epoch is monotonic, so entries are
    pushed in non-decreasing safe_at order -- a plain FIFO that stops at the first not-yet-ready
    entry is correct.  Slot 0 of the ring is reserved so head == tail unambiguously means empty.

    Overflow is handled rather than asserted: if the ring fills (thousands of destroys inside two
    epochs), vk_garbage_push falls back to a device wait-idle + full drain so it can never
    overflow or leak -- correct, just a one-off stall that should never occur in practice.

==============================================================================================*/

#define VK_GARBAGE_CAP 4096   /* ring capacity; (CAP - 1) entries usable at once */

typedef struct vk_garbage_s
{
    u32             safe_at;    /* global_epoch threshold at which destruction is safe      */
    u64             upload_wait;/* upload_timeline value that must be reached before freeing */
    VkImage         image;      /* any field may be VK_NULL_HANDLE / NULL when unused       */
    VkImageView     view;
    VkBuffer        buffer;
    VkSampler       sampler;
    VkDeviceMemory  memory;
    void*           mapped;     /* if non-NULL, memory is mapped and must be unmapped first */
} vk_garbage_t;

static vk_garbage_t g_garbage[ VK_GARBAGE_CAP ];
static u32          g_garbage_head;                 /* next entry to collect */
static u32          g_garbage_tail;                 /* next entry to write   */

/* Destroy one entry's objects immediately.  Order respects lifetime: unmap before free,
   view before its image. */

static void
vk_garbage_free_entry( vk_garbage_t* e )
{
    if ( e->mapped )                    vkUnmapMemory     ( vk.device, e->memory );
    if ( e->view    != VK_NULL_HANDLE ) vkDestroyImageView( vk.device, e->view,    vk.alloc_cb );
    if ( e->image   != VK_NULL_HANDLE ) vkDestroyImage    ( vk.device, e->image,   vk.alloc_cb );
    if ( e->buffer  != VK_NULL_HANDLE ) vkDestroyBuffer   ( vk.device, e->buffer,  vk.alloc_cb );
    if ( e->sampler != VK_NULL_HANDLE ) vkDestroySampler  ( vk.device, e->sampler, vk.alloc_cb );
    if ( e->memory  != VK_NULL_HANDLE ) vkFreeMemory      ( vk.device, e->memory,  vk.alloc_cb );

    *e = ( vk_garbage_t ){ 0 };
}

/* Free entries whose safe_at epoch has been reached.  force=true ignores the epoch and frees
   everything (used at shutdown and on the overflow fallback, both after a wait-idle). */

static void
vk_garbage_collect( bool force )
{
    /* Two independent reclaim gates, because a resource can be referenced from two queues on
       two different clocks:
         - graphics: bounded by global_epoch (advances only after every context's fence wait);
         - transfer: bounded by upload_timeline (a resource uploaded via staging is referenced
           by a transfer command buffer submitted at the next flush, NOT yet drained when
           global_epoch elapses -- this is what aggressive same-frame frees expose).
       An entry is freed only once BOTH have caught up.  Both safe_at and upload_wait are
       assigned monotonically in push order, so a plain FIFO stop-at-first-not-ready holds. */

    u64 upload_done = 0;
    if ( !force && vk.upload_timeline != VK_NULL_HANDLE )
        vkGetSemaphoreCounterValue( vk.device, vk.upload_timeline, &upload_done );

    while ( g_garbage_head != g_garbage_tail )
    {
        vk_garbage_t* e = &g_garbage[ g_garbage_head ];
        if ( !force && ( vk.global_epoch < e->safe_at || upload_done < e->upload_wait ) )
            break;  /* FIFO: every later entry has equal-or-larger thresholds, so stop here. */

        vk_garbage_free_entry( e );
        g_garbage_head = ( g_garbage_head + 1 ) % VK_GARBAGE_CAP;
    }
}

/* Queue a resource's backing objects for destruction once in-flight frames have drained.
   The caller passes its objects by value and clears its own slot; any zero field is ignored. */

static void
vk_garbage_push( const vk_garbage_t* objs )
{
    u32 next = ( g_garbage_tail + 1 ) % VK_GARBAGE_CAP;

    /* Ring full: drain everything safely so we never overflow.  Costs a full GPU stall, but
       only triggers under thousands of destroys between two frames -- effectively never.

       Flush any open upload batch FIRST.  A batch open here holds recorded-but-unsubmitted transfer
       copies whose destination buffers/images may be among the entries collect(true) is about to
       free; vkDeviceWaitIdle does not wait for work that was never submitted, so freeing them would
       destroy resources a later flush still references.  Submitting the batch now turns those copies
       into in-flight work the wait-idle below actually drains, making the force-collect safe. */
    if ( next == g_garbage_head )
    {
        LOG_WARN( "ring full (%d entries); forcing wait-idle drain", VK_GARBAGE_CAP - 1 );
        vk_upload_flush();
        vkDeviceWaitIdle( vk.device );
        vk_garbage_collect( true );
        next = ( g_garbage_tail + 1 ) % VK_GARBAGE_CAP;
    }

    /* Drop any pending/recorded QFOT acquire that names this resource: it is being destroyed and
       will never be sampled, so injecting an acquire for it later would reference a dead handle
       (hit when the pending list went undrained, e.g. every window minimized). */
    if ( objs->image  != VK_NULL_HANDLE ) vk_upload_forget_image ( objs->image  );
    if ( objs->buffer != VK_NULL_HANDLE ) vk_upload_forget_buffer( objs->buffer );

    g_garbage[ g_garbage_tail ] = *objs;

    /* Graphics-queue gate (safe_at).  The normal case -- the last graphics reference was a draw
       this epoch -- drains at global_epoch + FRAMES_IN_FLIGHT.  But a resource uploaded via
       staging this epoch also gets a QFOT acquire barrier injected into a graphics command
       buffer at the NEXT flush (one epoch later, see vk_upload_apply_acquires), so that graphics
       reference drains one epoch after a normal one.  upload_batch_open signals an open upload
       batch, so add that extra epoch to cover the deferred acquire. */
    u32 deferred_acquire = vk.upload_batch_open ? 1 : 0;
    g_garbage[ g_garbage_tail ].safe_at = vk.global_epoch + VK_MAX_FRAMES_IN_FLIGHT + deferred_acquire;

    /* Transfer-queue gate (upload_wait).  Backstop for the transfer copy itself: if a staged
       upload referencing this resource is still recording it completes at upload_counter+1
       (signaled by the next flush), otherwise the last flushed batch (upload_counter) covers it.
       safe_at usually implies this (the graphics frame that consumes the upload waits on the
       timeline before its fence signals), but this gate stays correct even when no graphics
       frame drains the transfer -- e.g. every context minimized the whole time. */
    g_garbage[ g_garbage_tail ].upload_wait = vk.upload_counter + ( vk.upload_batch_open ? 1 : 0 );

    g_garbage_tail = next;
}

/*============================================================================================*/
