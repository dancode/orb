/*==============================================================================================

    vulkan/vk_device.c -- VkDevice creation, queue retrieval, and subsystem orchestration.

    Called after vk_instance_create.  Physical device selection lives in
    vk_device_select.c (enumeration, feature validation, queue family discovery).
    This file takes over once a VkPhysicalDevice is committed to vk.*.

    Initialization phases (in order, all called from vk_device_create):
        vk_device_select        -- (vk_device_select.c) enumerate, validate, pick best
        vk_device_cache_limits  -- store key hardware limits in vk.*; log and validate
        vk_device_log_memory    -- log all memory types and heaps for allocation debugging

    Queue layout consumed by the rest of the RHI:
        graphics_queue   -- all draw calls, dynamic rendering, image layout transitions
        present_queue    -- vkQueuePresentKHR; shares the graphics family on most desktop HW
        transfer_queue   -- async staging uploads via vk_upload.c; separate DMA engine on
                            discrete AMD/NVIDIA reduces pipeline stalls; falls back to graphics

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Physical device limits cache and memory log
==============================================================================================*/

static void
vk_device_cache_limits( void )
{
    const VkPhysicalDeviceLimits* L = &vk.physical_device_props.limits;

    /* Resolve max MSAA to the highest single flag bit supported by both color and depth.
       Stored as VkSampleCountFlagBits so callers can pass it directly to Vulkan APIs. */
    VkSampleCountFlags msaa_mask = L->framebufferColorSampleCounts
                                 & L->framebufferDepthSampleCounts;

    VkSampleCountFlagBits max_msaa  = VK_SAMPLE_COUNT_1_BIT;
         if ( msaa_mask & VK_SAMPLE_COUNT_64_BIT ) max_msaa = VK_SAMPLE_COUNT_64_BIT;
    else if ( msaa_mask & VK_SAMPLE_COUNT_32_BIT ) max_msaa = VK_SAMPLE_COUNT_32_BIT;
    else if ( msaa_mask & VK_SAMPLE_COUNT_16_BIT ) max_msaa = VK_SAMPLE_COUNT_16_BIT;
    else if ( msaa_mask & VK_SAMPLE_COUNT_8_BIT  ) max_msaa = VK_SAMPLE_COUNT_8_BIT;
    else if ( msaa_mask & VK_SAMPLE_COUNT_4_BIT  ) max_msaa = VK_SAMPLE_COUNT_4_BIT;
    else if ( msaa_mask & VK_SAMPLE_COUNT_2_BIT  ) max_msaa = VK_SAMPLE_COUNT_2_BIT;
    
    vk.max_msaa_samples = max_msaa;
    vk.min_ubo_align    = (u32)L->minUniformBufferOffsetAlignment;

    LOG_INFO( "device limits:" );
    LOG_INFO( "  maxPushConstantsSize       : %u bytes (RHI needs %u)", L->maxPushConstantsSize, RHI_MAX_PUSH_CONST_SIZE );
    LOG_INFO( "  maxImageDimension2D        : %u",           L->maxImageDimension2D    );
    LOG_INFO( "  maxMemoryAllocationCount   : %u",           L->maxMemoryAllocationCount );
    LOG_INFO( "  minUniformBufferOffset     : %u bytes",     vk.min_ubo_align          );
    LOG_INFO( "  timestampPeriod            : %.3f ns/tick", L->timestampPeriod        );
    LOG_INFO( "  max MSAA (color+depth)     : %dx",          (u32)vk.max_msaa_samples  );

    /* Warn early if push constants would be rejected by vkCreatePipelineLayout. */
    if ( L->maxPushConstantsSize < RHI_MAX_PUSH_CONST_SIZE )
    {
        LOG_WARN( "  maxPushConstantsSize (%u) < RHI_MAX_PUSH_CONST_SIZE (%u); "
                  "push constant range in vk_descriptor.c must be reduced",
                  L->maxPushConstantsSize, RHI_MAX_PUSH_CONST_SIZE );
    }
}

/*==============================================================================================
     Physical Device: Expose the memory type indexes for clarity (pure log output function)
==============================================================================================*/

static void
vk_device_log_memory( void )
{
    const VkPhysicalDeviceMemoryProperties* mp = &vk.memory_props;

    LOG_TRACE( "memory heaps (%u):", mp->memoryHeapCount );
    for ( u32 i = 0; i < mp->memoryHeapCount; ++i )
    {
        VkMemoryHeapFlags f  = mp->memoryHeaps[ i ].flags;
        double gb = (double)mp->memoryHeaps[ i ].size / ( 1024.0 * 1024.0 * 1024.0 );

        LOG_TRACE( "  heap[%u]: %6.2f GB  %s%s", i, gb,
                   ( f & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT   ) ? "DEVICE_LOCAL "   : "",
                   ( f & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ) ? "MULTI_INSTANCE " : "" );
    }

    LOG_TRACE( "memory types (%u):", mp->memoryTypeCount );
    for ( u32 i = 0; i < mp->memoryTypeCount; ++i )
    {
        VkMemoryPropertyFlags f = mp->memoryTypes[ i ].propertyFlags;
        u32                   h = mp->memoryTypes[ i ].heapIndex;

        if ( f == 0 )
            continue;

        /* Append only the set bits; avoids a wide fixed-column table full of dashes. */
        char buf[ 256 ] = { 0 };
        int  pos = 0;
        if ( f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT     ) pos += snprintf( buf+pos, 256-pos, "DEVICE_LOCAL "     );
        if ( f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT     ) pos += snprintf( buf+pos, 256-pos, "HOST_VISIBLE "     );
        if ( f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT    ) pos += snprintf( buf+pos, 256-pos, "HOST_COHERENT "    );
        if ( f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT      ) pos += snprintf( buf+pos, 256-pos, "HOST_CACHED "      );
        if ( f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ) pos += snprintf( buf+pos, 256-pos, "LAZILY_ALLOCATED " );
        if ( f & VK_MEMORY_PROPERTY_PROTECTED_BIT        ) pos += snprintf( buf+pos, 256-pos, "PROTECTED "        );

        LOG_TRACE( "  type[%u] heap=%u : %s", i, h, buf );
    }
}

/*==============================================================================================

    Logical Device: Helpers for creating and managing logical devices

==============================================================================================*/
/*==============================================================================================
    Logical Device: Set features and extensions for device creation.

    This wires with the pNext links chain orders from bottom of code to the top.
        e.g. desc_idx <- dyn_rend <- sync2 <- timeline <- feats2.
==============================================================================================*/

static void
vk_device_init_features( vk_feature_chain_t* f )
{
    //-------------------------------------------------------
    f->desc_idx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    
    /* per-lane dynamic index into bindless array */
    f->desc_idx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;  

    /* bindless slots may be empty/unused */
    f->desc_idx.descriptorBindingPartiallyBound = VK_TRUE;

    /* UPDATE_AFTER_BIND on sampled image + sampler bindings -- required by bindless layout.
       Spec covers VK_DESCRIPTOR_TYPE_SAMPLER under this same flag. */
    f->desc_idx.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

    /* write new slots while GPU reads others */
    f->desc_idx.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

    /* array size set by layout, not SPIR-V */
    f->desc_idx.runtimeDescriptorArray = VK_TRUE;  
    
    //-------------------------------------------------------
    f->bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    f->bda.pNext = &f->desc_idx;

    /* 64-bit GPU virtual addresses; enables vkGetBufferDeviceAddress for BDA buffers */
    f->bda.bufferDeviceAddress = VK_TRUE;

    //-------------------------------------------------------
    f->dyn_rend.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    f->dyn_rend.pNext = &f->bda;

    /* vkCmdBeginRendering -- no VkRenderPass / VkFramebuffer */
    f->dyn_rend.dynamicRendering = VK_TRUE;
    
    //-------------------------------------------------------
    f->sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    f->sync2.pNext = &f->dyn_rend;    
    
    /* vkCmdPipelineBarrier2 + vkQueueSubmit2 */
    f->sync2.synchronization2 = VK_TRUE;  
    
    //-------------------------------------------------------
    f->timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    f->timeline.pNext = &f->sync2;    

    /* monotonically increasing GPU/CPU sync counter */
    f->timeline.timelineSemaphore = VK_TRUE; 

    //-------------------------------------------------------
    f->feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f->feats2.pNext = &f->timeline;

    /* anisotropic texture filtering */
    f->feats2.features.samplerAnisotropy = VK_TRUE;  

    /* VK_POLYGON_MODE_LINE wireframe */
    f->feats2.features.fillModeNonSolid  = VK_TRUE;  
}

/*==============================================================================================
    Logical Device: Build the extension list for VkDeviceCreateInfo.  
    
    We prepend the required swapchain extension (always required) and then appends 
    each optional extension reported present by opt_found[]. Returns the total count.
==============================================================================================*/

static u32
vk_device_collect_extensions( const bool* opt_found, const char** out_exts )
{
    u32 count        = 0;
    out_exts[ count++ ] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;   /* hard requirement */
    
    LOG_INFO( "optional extentions:" );

    for ( u32 i = 0; i < VK_OPT_EXT_COUNT; ++i )
    {
        LOG_INFO( "  [%u]: %s [%s]", i, s_optional_exts[ i ], opt_found[ i ] ? "enabled" : "not present" );

        if ( opt_found[ i ] ) 
             out_exts[ count++ ] = s_optional_exts[ i ];
    }
    return count;
}

/*==============================================================================================
    Logical Device: Build the queue create info array for vkCreateDevice.

    Build unique queue info array for vkCreateDevice. Deduplicates indices—common when
    Graphics/Present/Transfer share hardware—to avoid the Vulkan validation error
    of providing the same family index twice. Returns unique entry count

    - Graphics and present share one family on most desktop hardware.
    - Transfer may or may not be dedicated.
    - Duplicate indices are collapsed -- passing the same index twice is a validation error.
    - Returns the number of unique entries written to out_cis.

==============================================================================================*/

/* Queue priorities at file scope so pQueuePriorities pointers in VkDeviceQueueCreateInfo
   remain valid through vkCreateDevice call without escaping a helper's stack frame. */

static const float s_prio_high = 1.0f;   /* graphics + present: highest scheduling weight */
static const float s_prio_low  = 0.5f;   /* transfer (dedicated): lower than rendering   */

static u32
vk_device_build_queue_infos( VkDeviceQueueCreateInfo out_cis[ 3 ] )
{
    const u32 family_array[ 3 ] = 
        { vk.graphics_queue_family, vk.present_queue_family, vk.transfer_queue_family };
    const f32* queue_priority[ 3 ] = 
        { &s_prio_high, &s_prio_high, &s_prio_low };

    u32 count = 0;
    for ( u32 i = 0; i < 3; ++i )
    {
        u32 family_id = family_array[ i ];

        bool dup = false;
        for ( u32 j = 0; j < count; ++j )
        {
            if ( out_cis[ j ].queueFamilyIndex == family_id ) { dup = true; break; }
        }
        if ( dup ) continue;

        out_cis[ count ]                  = ( VkDeviceQueueCreateInfo ){ 0 };
        out_cis[ count ].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        out_cis[ count ].queueFamilyIndex = family_id;
        out_cis[ count ].queueCount       = 1;
        out_cis[ count ].pQueuePriorities = queue_priority[ i ];
        ++count;
    }
    return count;
}

/*==============================================================================================
    vk_device_wait_idle  --  drain all queues on the logical device.
    Call before destroying any device-owned resource to ensure GPU work is complete.
==============================================================================================*/

static void
vk_device_wait_idle( void )
{
    if ( vk.device == VK_NULL_HANDLE )
        return;

    VkResult result = vkDeviceWaitIdle( vk.device );
    if ( result != VK_SUCCESS )
        LOG_ERROR( "vkDeviceWaitIdle: %s", string_VkResult( result ) );
}

/*==============================================================================================
    vk_device_create  --  top-level orchestrator for physical and logical device init.
==============================================================================================*/

static bool
vk_device_create( void )
{
    /* Phase 1: Find the RHI hardware device that supports our features */

    bool optional_ext_found[ VK_OPT_EXT_COUNT ] = { 0 };
    if ( vk_device_select( optional_ext_found ) == false ) 
    {
        return false; /* no suitable device found */
    }

    /* Phase 2: Cache hardware limits in vk.* and log them. */

    vk_device_cache_limits();   // cache key limits + visual output
    vk_device_log_memory();     // visual output only.

    /* Phase 3: Build extension list from opt_found; record capability flags. */

    const char* dev_exts[ 1 + VK_OPT_EXT_COUNT ];
    u32 dev_ext_count = vk_device_collect_extensions( optional_ext_found, dev_exts );
    vk.has_push_descriptor   = optional_ext_found[ VK_OPT_EXT_PUSH_DESCRIPTOR ];
    vk.has_fifo_latest_ready = optional_ext_found[ VK_OPT_EXT_FIFO_LATEST_READY ];

    /* Phase 4: Fill required feature pNext chain. */

    vk_feature_chain_t features = { 0 };
    vk_device_init_features( &features );

    /* Wire optional feature structs for extensions that were found. Each is prepended
       to the chain so it survives past this scope without escaping a stack frame. */
    if ( optional_ext_found[ VK_OPT_EXT_FIFO_LATEST_READY ] )
    {
        features.fifo_latest_ready.sType                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
        features.fifo_latest_ready.presentModeFifoLatestReady = VK_TRUE;
        features.fifo_latest_ready.pNext                      = features.feats2.pNext;
        features.feats2.pNext                                 = &features.fifo_latest_ready;
    }

    /* Phase 5: Build deduplicated queue create infos. */

    VkDeviceQueueCreateInfo qcis[ 3 ];
    u32 qci_count = vk_device_build_queue_infos( qcis );

    /* Phase 6: Create the logical device.
       pEnabledFeatures must be NULL when VkPhysicalDeviceFeatures2 is in pNext --
       setting both is a validation error; pNext is the authoritative path for VK 1.1+. */

    VkDeviceCreateInfo ci          = { 0 };
    ci.sType                       = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                       = &features.feats2;
    ci.queueCreateInfoCount        = qci_count;
    ci.pQueueCreateInfos           = qcis;
    ci.enabledExtensionCount       = dev_ext_count;
    ci.ppEnabledExtensionNames     = dev_exts;

    VkResult result = vkCreateDevice( vk.physical_device, &ci, vk.alloc_cb, &vk.device );
    if ( result != VK_SUCCESS )
    {
        LOG_ERROR( "vkCreateDevice: %s", string_VkResult( result ) );
        return false;
    }

    /* Device-level function pointers; required before any vkCmd* or resource call. */

    if ( !vk_lib_device_entry_points() )
    {
        vkDestroyDevice( vk.device, vk.alloc_cb );
        vk.device = VK_NULL_HANDLE;
        return false;
    }

    /* Retrieve queue handles. When families overlap all three may alias the same VkQueue
       object -- the driver serializes them internally; this is valid Vulkan. */

    vkGetDeviceQueue( vk.device, vk.graphics_queue_family, 0, &vk.graphics_queue );
    vkGetDeviceQueue( vk.device, vk.present_queue_family,  0, &vk.present_queue  );
    vkGetDeviceQueue( vk.device, vk.transfer_queue_family, 0, &vk.transfer_queue  );

    /* Subsystem init in dependency order: pipeline cache -> descriptor -> upload. */

    vk_pipeline_cache_load();

    if ( !vk_descriptor_init() )
        goto fail_after_cache;

    if ( !vk_upload_init() )
        goto fail_after_descriptor;

    LOG_INFO( "vk_device_create: OK" );
    return true;

fail_after_descriptor:  vk_descriptor_shutdown();
fail_after_cache:       vk_pipeline_cache_save();

    if ( vk.pipeline_cache != VK_NULL_HANDLE )
    {
        vkDestroyPipelineCache( vk.device, vk.pipeline_cache, vk.alloc_cb );
        vk.pipeline_cache = VK_NULL_HANDLE;
    }
    vkDestroyDevice( vk.device, vk.alloc_cb );
    vk.device = VK_NULL_HANDLE;
    return false;
}

/*==============================================================================================
    vk_device_destroy
==============================================================================================*/

static void
vk_device_destroy( void )
{
    if ( vk.device == VK_NULL_HANDLE )
        return;

    /* Drain all queues before touching any device-owned object.  Skipping this causes
       validation errors and potential crashes if the GPU is mid-flight. */
    vk_device_wait_idle();

    /* Reverse init order: upload holds staging buffers in device memory, so it tears down
       before descriptor (pipeline layout + bindless pool), which tears down before the device. */
    vk_upload_shutdown();
    vk_descriptor_shutdown();

    /* Save pipeline cache so the next session skips driver shader recompilation. */
    vk_pipeline_cache_save();
    if ( vk.pipeline_cache != VK_NULL_HANDLE )
    {
        vkDestroyPipelineCache( vk.device, vk.pipeline_cache, vk.alloc_cb );
        vk.pipeline_cache = VK_NULL_HANDLE;
    }

    vkDestroyDevice( vk.device, vk.alloc_cb );
    vk.device = VK_NULL_HANDLE;
}

/*============================================================================================*/
// clang-format on
