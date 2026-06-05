/*==============================================================================================

    vulkan/vk_device.c -- VkPhysicalDevice selection + VkDevice + queues.

    Called after vk_instance_create.  Physical device selection uses the platform
    presentation support query (vkGetPhysicalDeviceWin32PresentationSupportKHR etc.)
    as a proxy for surface support so no surface is needed at device-select time.
    The first context_create call validates the choice against a real surface.

    Initialization phases (in order, all called from vk_device_create):
        vk_device_select        -- enumerate GPUs, score, pick best, store in vk.*
        vk_device_cache_limits  -- store key hardware limits in vk.*; log and validate
        vk_device_log_memory    -- log all memory types and heaps for allocation debugging

    Required Vulkan 1.3 features (device rejected if absent):
        dynamicRendering        -- vkCmdBeginRendering; eliminates VkRenderPass objects
        synchronization2        -- vkCmdPipelineBarrier2 / vkQueueSubmit2 (VkDependencyInfo)
        timelineSemaphore       -- monotonic GPU/CPU sync counter; no polling loops needed

    Required Vulkan 1.2 features (device rejected if absent):
        descriptorIndexing:
            shaderSampledImageArrayNonUniformIndexing  -- shader can index bindless array
                                                          with a non-uniform (per-lane) index
            descriptorBindingPartiallyBound            -- bindless slots may be empty; shaders
                                                          must not read them, but the array can
                                                          have holes
            descriptorBindingSampledImageUpdateAfterBind -- UPDATE_AFTER_BIND on sampled image
                                                          and sampler bindings (spec covers
                                                          VK_DESCRIPTOR_TYPE_SAMPLER here too)
            descriptorBindingUpdateUnusedWhilePending  -- CPU can write new descriptors into
                                                          unused slots while GPU reads other slots
                                                          in the same array; enables streaming
            runtimeDescriptorArray                     -- array size comes from the pipeline
                                                          layout, not baked into SPIR-V

    Required VkPhysicalDeviceFeatures (device rejected if absent):
        samplerAnisotropy   -- anisotropic filtering on textures at oblique viewing angles
        fillModeNonSolid    -- VK_POLYGON_MODE_LINE for wireframe debug overlays

    Optional extensions (enabled when present, ignored otherwise):
        VK_KHR_push_descriptor  -- vkCmdPushDescriptorSetKHR: inline descriptor updates
                                   per command, avoids pool/set allocation for per-draw data

    Queue layout consumed by the rest of the RHI:
        graphics_queue   -- all draw calls, dynamic rendering, image layout transitions
        present_queue    -- vkQueuePresentKHR; shares the graphics family on most desktop HW
        transfer_queue   -- async staging uploads via vk_upload.c; separate DMA engine on
                            discrete AMD/NVIDIA reduces pipeline stalls; falls back to graphics

==============================================================================================*/
// clang-format off

/*==============================================================================================

    Physical Device: Helpers for creating and managing physical devices

==============================================================================================*/

#define VK_MAX_PHYSICAL_DEVICES     8
#define VK_MAX_QUEUE_FAMILIES       16
#define VK_QUEUE_FAMILY_INVALID     ( ~0u )

/* Optional extensions: checked against the selected device; enabled only if present.
   Each entry has a named index in vk_opt_ext_e; VK_OPT_EXT_COUNT is the sentinel.
   Reference slots by name (e.g. VK_OPT_EXT_PUSH_DESCRIPTOR) -- never by raw integer. */

enum
{
    VK_OPT_EXT_PUSH_DESCRIPTOR = 0,   /* VK_KHR_push_descriptor: inline per-draw updates */
    VK_OPT_EXT_COUNT,
};

static const char* s_optional_exts[ VK_OPT_EXT_COUNT ] =
{
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

/* All pNext-linked feature structs together in one allocation so the chain can be built
   by a helper without the pointers escaping its stack frame. feats2 is the chain root. */

typedef struct
{
    VkPhysicalDeviceDescriptorIndexingFeatures  desc_idx;  // bindless indexing: VK 1.2 
    VkPhysicalDeviceDynamicRenderingFeatures    dyn_rend;  // renderpass-free: VK 1.3   
    VkPhysicalDeviceSynchronization2Features    sync2;     // barrier2/submit2: VK 1.3  
    VkPhysicalDeviceTimelineSemaphoreFeatures   timeline;  // monotonic counter: VK 1.2 
    VkPhysicalDeviceFeatures2                   feats2;    // chain root + VK 1.0 feats 

} vk_feature_chain_t;

/*==============================================================================================
    Physical Device: Check if device exposes VK_KHR_swapchain extension (hard requirement).

        Also checks which optional extensions are present and writes results to
        out_optional[] (one bool per entry, parallel to the array).

        Note: Extension counts reach 200+ on current drivers, so we heap-allocate
              rather than risk a large stack frame.
==============================================================================================*/

static bool
vk_device_validate_extensions( VkPhysicalDevice dev, bool* out_optional )
{
    /* Query device extensions + store in heap-allocated array */

    u32 count = 0;
    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, NULL );
    VkExtensionProperties* exts = malloc( sizeof( VkExtensionProperties ) * ( count + 1 ) );
    if ( !exts ) return false;
    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, exts );

    /* Check for required + optional extension support */

    bool has_swapchain = false;
    for ( u32 i = 0; i < count; ++i )
    {
        const char* name = exts[ i ].extensionName;

        if ( strcmp( name, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 ) {
             has_swapchain = true;
        }
        for ( u32 j = 0; j < VK_OPT_EXT_COUNT; ++j ) {
            if ( strcmp( name, s_optional_exts[ j ] ) == 0 )
                out_optional[ j ] = true;
        }
    }
    free( exts );
    return has_swapchain;
}

/*==============================================================================================
    Physical Device: Get queue family indicices for graphics + compute, present, transfer.

    Combined graphics + compute:
       Render and compute share the same timeline so they can occupy the same command buffer
       without cross-family ownership transfers or extra pipeline barriers.

    Dedicated transfer:
       Discrete AMD and NVIDIA expose a DMA-only queue family that runs in parallel with the
       3D engine.  Staging uploads on this family avoid stalls on the graphics queue during
       asset streaming.  Falls back to graphics on hardware without a dedicated copy engine.

    Surface-free present query:
       vkGetPhysicalDeviceSurfaceSupportKHR requires a live VkSurfaceKHR which does not exist
       at device-select time.  The Win32 platform query answers without one.  context_create
       re-validates via vkGetPhysicalDeviceSurfaceSupportKHR once a real surface is available.

    Returns false if graphics or present cannot be satisfied (both are hard requirements).
==============================================================================================*/

static bool
vk_device_validate_queues( VkPhysicalDevice dev, u32* out_gfx, u32* out_present, u32* out_transfer )
{
    VkQueueFamilyProperties queue_families[ VK_MAX_QUEUE_FAMILIES ];

    u32 count    = VK_MAX_QUEUE_FAMILIES;
    u32 gfx      = VK_QUEUE_FAMILY_INVALID;
    u32 present  = VK_QUEUE_FAMILY_INVALID;
    u32 transfer = VK_QUEUE_FAMILY_INVALID;

    vkGetPhysicalDeviceQueueFamilyProperties( dev, &count, queue_families );

    for ( u32 i = 0; i < count; ++i )
    {
        VkQueueFlags flags = queue_families[ i ].queueFlags;

        /* Combined graphics + compute: both are needed for our command buffers, 
           so prefer a single family that supports both. */

        if ( gfx == VK_QUEUE_FAMILY_INVALID && ( flags & VK_QUEUE_GRAPHICS_BIT ) && 
                                               ( flags & VK_QUEUE_COMPUTE_BIT )) {
             gfx = i;
        }

        /* Use the surface-free present support query (platform-specific) to
           check for surface capability, which is confirmed in context creation */

        if ( present == VK_QUEUE_FAMILY_INVALID )
        {
            #if defined( VK_USE_PLATFORM_WIN32_KHR )
                // Surface-free query can answer without a HWND or VkSurfaceKHR.
                // On most Win32 drivers the graphics family also supports present.
                if ( vkGetPhysicalDeviceWin32PresentationSupportKHR( dev, i ) )
                    present = i;
            #else
                // Lnux/Mac: has no surface-free present query is available without a
                // live display connection.  Accept any graphics family; context_create 
                // will confirm support via vkGetPhysicalDeviceSurfaceSupportKHR.
                if ( flags & VK_QUEUE_GRAPHICS_BIT )
                    present = i;
            #endif
        }

        /* Dedicated transfer: TRANSFER set, GRAPHICS and COMPUTE both absent.
           Maps to copy-engine / DMA hardware on discrete GPUs. */
        if ( transfer == VK_QUEUE_FAMILY_INVALID &&  ( flags & VK_QUEUE_TRANSFER_BIT ) && 
             !( flags & VK_QUEUE_GRAPHICS_BIT  ) && !( flags & VK_QUEUE_COMPUTE_BIT  )) 
        {
            transfer = i;
        }
    }
    
    /* Integrated GPUs and older discrete cards expose no dedicated transfer family.
       Fall back to the graphics family so transfer_queue is always a valid handle. */
    if ( transfer == VK_QUEUE_FAMILY_INVALID )
         transfer = gfx;

    *out_gfx      = gfx;
    *out_present  = present;
    *out_transfer = transfer;
    
    return ( gfx != VK_QUEUE_FAMILY_INVALID ) && ( present != VK_QUEUE_FAMILY_INVALID );
}

/*==============================================================================================
    Physical Device: Full compatibility check for a device. False if requirements fail.
==============================================================================================*/

static bool
vk_device_validate( VkPhysicalDevice dev, u32* 
                    out_gfx, u32* out_present, u32* out_transfer, bool* out_opt )
{
    /* validate extensions */
    if ( !vk_device_validate_extensions( dev, out_opt ))
         return false;

    /* validate physical device features -- chain all required 1.0/1.2/1.3 structs so
       vkGetPhysicalDeviceFeatures2 populates every flag in one call, then reject the
       device if any hard requirement is absent. */

    vk_feature_chain_t f    = { 0 };
    f.desc_idx.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    f.dyn_rend.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    f.dyn_rend.pNext        = &f.desc_idx;
    f.sync2.sType           = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    f.sync2.pNext           = &f.dyn_rend;
    f.timeline.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    f.timeline.pNext        = &f.sync2;
    f.feats2.sType          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f.feats2.pNext          = &f.timeline;

    vkGetPhysicalDeviceFeatures2( dev, &f.feats2 );

    /* VK 1.0 requirements */
    if ( !f.feats2.features.samplerAnisotropy ||
         !f.feats2.features.fillModeNonSolid ) {
        return false;
    }

    /* VK 1.2 descriptor indexing requirements */
    if ( !f.desc_idx.shaderSampledImageArrayNonUniformIndexing    ||
         !f.desc_idx.descriptorBindingPartiallyBound              ||
         !f.desc_idx.descriptorBindingSampledImageUpdateAfterBind ||
         !f.desc_idx.descriptorBindingUpdateUnusedWhilePending    ||
         !f.desc_idx.runtimeDescriptorArray ) {
        return false;
    }

    /* VK 1.3 requirements */
    if ( !f.dyn_rend.dynamicRendering ||
         !f.sync2.synchronization2    ||
         !f.timeline.timelineSemaphore ) {
        return false;
    }

    /* validate queue families */
    return vk_device_validate_queues( dev, out_gfx, out_present, out_transfer );
}

/*==============================================================================================
    Physical Device: Pick the best (GPU) device and store in vulkan state struct.
==============================================================================================*/

static bool
vk_device_select( bool* out_opt_found )
{
    /* Enumerate vulkan capable devices on machine */

    VkPhysicalDevice            device_array[ VK_MAX_PHYSICAL_DEVICES ];
    VkPhysicalDeviceProperties  device_props[ VK_MAX_PHYSICAL_DEVICES ];
    u32                         device_count = VK_MAX_PHYSICAL_DEVICES;

    VkResult result = vkEnumeratePhysicalDevices( vk.instance, &device_count, device_array );
    if (( result != VK_SUCCESS && result != VK_INCOMPLETE ) || device_count == 0 ) {
        LOG_ERROR( "vkEnumeratePhysicalDevices: %s (count=%u)", string_VkResult( result ), device_count );
        return false;
    }

    LOG_INFO( "physical devices found: %u", device_count );

    /* Fetch device properties for all devices */    
    for ( u32 i = 0; i < device_count; ++i )
        vkGetPhysicalDeviceProperties( device_array[ i ], &device_props[ i ] );
    
    for ( u32 i = 0; i < device_count; ++i ) {
        LOG_INFO( "[%u] %s type=%s api=%u.%u", i,
                  device_props[ i ].deviceName, string_VkPhysicalDeviceType( device_props[ i ].deviceType ),
                  VK_VERSION_MAJOR( device_props[ i ].apiVersion ), VK_VERSION_MINOR( device_props[ i ].apiVersion ) );
    }

    /* Preferred device by type */
    static const VkPhysicalDeviceType s_pref[] = {
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,  VK_PHYSICAL_DEVICE_TYPE_CPU,
        VK_PHYSICAL_DEVICE_TYPE_OTHER,
    };
    
    /* Validate first successful device in order of type preference */
    u32 pref_count = sizeof( s_pref ) / sizeof( s_pref[ 0 ] );
    for ( u32 t = 0; t < pref_count; ++t )
    {
        for ( u32 i = 0; i < device_count; ++i )
        {
            // Skip if not the preferred type for this iteration.
            if ( device_props[ i ].deviceType != s_pref[ t ] )
                 continue;

            // Check if the device meets our requirements.
            bool opt[ VK_OPT_EXT_COUNT ] = { 0 };
            u32  gfx, pres, xfer;

            // We grab the queue family values since we are validating them anyway.
            if ( !vk_device_validate( device_array[ i ], &gfx, &pres, &xfer, opt )) {
                 LOG_INFO( "  skipped %s (failed compatibility check)", device_props[ i ].deviceName );
                 continue;
            }

            // Store our selection.
            vk.physical_device       = device_array[ i ];
            vk.physical_device_props = device_props[ i ];
            vk.graphics_queue_family = gfx;
            vk.present_queue_family  = pres;
            vk.transfer_queue_family = xfer;
            vkGetPhysicalDeviceMemoryProperties( vk.physical_device, &vk.memory_props );

            // Store which optional extensions were present for later.
            memcpy( out_opt_found, opt, VK_OPT_EXT_COUNT * sizeof( bool ) );

            LOG_INFO( "best match: [%u]", i );
            LOG_INFO( "  device named : %s", vk.physical_device_props.deviceName );
            LOG_INFO( "  device type  : %s", string_VkPhysicalDeviceType( vk.physical_device_props.deviceType ));
            LOG_INFO( "  queue family : Graphics = %u Present = %u Transfer = %u", gfx, pres, xfer );
            LOG_INFO( "  api          : %s", vk_version_string( vk.physical_device_props.apiVersion ));
            LOG_INFO( "  driver       : %s", vk_version_string( vk.physical_device_props.driverVersion ));
            LOG_INFO( "  vendor id    : %u", vk.physical_device_props.vendorID );                                                                 
            
            return true; /* success */
        }
    }

    LOG_ERROR( "no suitable physical device requires VK_KHR_swapchain, samplerAnisotropy, "
               "fillModeNonSolid, graphics + compute queue, present support)" );
    return false;
}

/*==============================================================================================
    Physical Device: Cache key limits in our vulkan state struct.
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
    f->dyn_rend.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    f->dyn_rend.pNext = &f->desc_idx;

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
    
    Prepends the required swapchain extension, then appends each optional extension 
    reported present by opt_found[]. 
    
    Returns total count. 
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
    
    - Build deduplicated VkDeviceQueueCreateInfo entries for the three queue families.
    - Graphics and present share one family on most desktop hardware; 
    - Transfer may or may not be dedicated.      
    - Duplicate indices are collapsed -- passing the same index twice is a validation error. 
    - Returns the number of unique entries written to out_cis.

==============================================================================================*/

/* Queue priorities at file scope so pQueuePriorities pointers in VkDeviceQueueCreateInfo
   remain valid through vkCreateDevice without escaping a helper's stack frame. */

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
    vk.has_push_descriptor = optional_ext_found[ VK_OPT_EXT_PUSH_DESCRIPTOR ];

    /* Phase 4: Fill required feature pNext chain. */

    vk_feature_chain_t features = { 0 };
    vk_device_init_features( &features );

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

    /* Retrieve queue handles.  When families overlap all three may alias the same VkQueue
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

    LOG_INFO( "vkCreateDevice: OK" );
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
