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

#define VK_MAX_PHYSICAL_DEVICES     8
#define VK_MAX_QUEUE_FAMILIES       16
#define VK_QUEUE_FAMILY_INVALID     ( ~0u )

/* Optional extensions: checked against the selected device; enabled only if present.
   VK_OPT_EXT_COUNT is the compile-time array length -- use it instead of sizeof/sizeof. */

static const char* s_optional_exts[] =
{
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,   /* inline per-draw descriptor updates */
};

#define VK_OPT_EXT_COUNT  ( sizeof( s_optional_exts ) / sizeof( s_optional_exts[ 0 ] ) )

/* Queue priorities at file scope so pQueuePriorities pointers in VkDeviceQueueCreateInfo
   remain valid through vkCreateDevice without escaping a helper's stack frame. */

static const float s_prio_high = 1.0f;   /* graphics + present: highest scheduling weight */
static const float s_prio_low  = 0.5f;   /* transfer (dedicated): lower than rendering   */

/*==============================================================================================
    Physical device helpers
==============================================================================================*/

/* Returns true if dev exposes VK_KHR_swapchain (hard requirement; without it the device
   cannot present to any window).  Also checks which optional extensions from s_optional_exts[]
   are present and writes results to out_optional[] (one bool per entry, parallel to the array).

   Two-call pattern: first get count, then fill.  Extension counts reach 200+ on current
   drivers, so we heap-allocate rather than risk a large stack frame. */

static bool
vk_device_check_extensions( VkPhysicalDevice dev, bool* out_optional )
{
    /* -- Query device extensions -- */

    u32 count = 0;
    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, NULL );
    VkExtensionProperties* exts = malloc( sizeof( VkExtensionProperties ) * ( count + 1 ) );
    if ( !exts )
        return false;

    /* --- Store extension properties in heap-allocated array --- */

    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, exts );

    /* --- Check for required and optional extensions -- */

    bool has_swapchain = false;
    for ( u32 i = 0; i < count; ++i )
    {
        const char* name = exts[ i ].extensionName;

        if ( strcmp( name, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 )
            has_swapchain = true;

        for ( u32 j = 0; j < VK_OPT_EXT_COUNT; ++j )
        {
            if ( strcmp( name, s_optional_exts[ j ] ) == 0 )
                out_optional[ j ] = true;
        }
    }

    free( exts );
    return has_swapchain;
}

/*----------------------------------------------------------------------------------------------
    Walks queue families on dev and resolves indices for graphics+compute, present, transfer.

    Combined graphics+compute:
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
----------------------------------------------------------------------------------------------*/

static bool
vk_device_find_queues( VkPhysicalDevice dev,
                       u32* out_gfx, u32* out_present, u32* out_transfer )
{
    VkQueueFamilyProperties families[ VK_MAX_QUEUE_FAMILIES ];
    u32 count = VK_MAX_QUEUE_FAMILIES;
    vkGetPhysicalDeviceQueueFamilyProperties( dev, &count, families );

    u32 gfx      = VK_QUEUE_FAMILY_INVALID;
    u32 present  = VK_QUEUE_FAMILY_INVALID;
    u32 transfer = VK_QUEUE_FAMILY_INVALID;

    for ( u32 i = 0; i < count; ++i )
    {
        VkQueueFlags flags = families[ i ].queueFlags;

        if ( gfx == VK_QUEUE_FAMILY_INVALID &&
             ( flags & VK_QUEUE_GRAPHICS_BIT ) && ( flags & VK_QUEUE_COMPUTE_BIT ) )
        {
            gfx = i;
        }

        if ( present == VK_QUEUE_FAMILY_INVALID )
        {
#if defined( VK_USE_PLATFORM_WIN32_KHR )
            /* Surface-free query: answers without a HWND or VkSurfaceKHR.
               On most Win32 drivers the graphics family also supports present. */
            if ( vkGetPhysicalDeviceWin32PresentationSupportKHR( dev, i ) )
                present = i;
#else
            /* Linux/Mac: no surface-free present query is available without a live
               display connection.  Accept any graphics family; context_create will
               confirm support via vkGetPhysicalDeviceSurfaceSupportKHR. */
            if ( flags & VK_QUEUE_GRAPHICS_BIT )
                present = i;
#endif
        }

        /* Dedicated transfer: TRANSFER set, GRAPHICS and COMPUTE both absent.
           Maps to copy-engine / DMA hardware on discrete GPUs. */
        if ( transfer == VK_QUEUE_FAMILY_INVALID &&
             ( flags & VK_QUEUE_TRANSFER_BIT  ) &&
            !( flags & VK_QUEUE_GRAPHICS_BIT  ) &&
            !( flags & VK_QUEUE_COMPUTE_BIT   ) )
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

/*----------------------------------------------------------------------------------------------
    Scores a physical device.  Returns -1 if any hard requirement fails, otherwise
    a positive integer -- caller picks the maximum.

    Score bands:
        1000 -- discrete GPU (dedicated VRAM; preferred target for the game engine)
         100 -- integrated GPU (shared system memory; acceptable fallback)
           1 -- virtual / software / unknown type (CI, WSL2, RenderDoc replays)
----------------------------------------------------------------------------------------------*/

static int
vk_device_score( VkPhysicalDevice dev, u32* out_gfx, u32* out_present, u32* out_transfer,
                 bool* out_opt )
{
    /* --- Check for required features and extensions --- */

    if ( !vk_device_check_extensions( dev, out_opt ) )
        return -1; /* failed to meet requirements */

    VkPhysicalDeviceFeatures2 feats2 = { 0 };
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vkGetPhysicalDeviceFeatures2( dev, &feats2 );
    if ( !feats2.features.samplerAnisotropy || !feats2.features.fillModeNonSolid )
        return -1;

    if ( !vk_device_find_queues( dev, out_gfx, out_present, out_transfer ) )
        return -1;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties( dev, &props );

    if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ) return 1000;
    if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) return  100;
    return 1;
}

/*==============================================================================================
    vk_device_select  --  enumerate, score, pick best GPU; store in vk.*
==============================================================================================*/

static bool
vk_device_select( bool* out_opt_found )
{
    VkPhysicalDevice candidates[ VK_MAX_PHYSICAL_DEVICES ];
    u32 count = VK_MAX_PHYSICAL_DEVICES;

    /* A GPU is required (VK_INCOMPLETE a rare exception when 8+ GPUs installed) */
    VkResult result = vkEnumeratePhysicalDevices( vk.instance, &count, candidates );
    if (( result != VK_SUCCESS && result != VK_INCOMPLETE ) || count == 0 ) {
        LOG_ERROR( "vkEnumeratePhysicalDevices: %s (count=%u)", string_VkResult( result ), count );
        return false;
    }

    LOG_INFO( "physical devices found: %u", count );

    VkPhysicalDevice           best       = VK_NULL_HANDLE;
    u32                        best_gfx   = VK_QUEUE_FAMILY_INVALID;
    u32                        best_pres  = VK_QUEUE_FAMILY_INVALID;
    u32                        best_xfer  = VK_QUEUE_FAMILY_INVALID;
    VkPhysicalDeviceProperties best_props = { 0 };
    int                        best_score = -1;
    bool                       best_opt[ VK_OPT_EXT_COUNT ] = { 0 };

    for ( u32 i = 0; i < count; ++i )
    {
        VkPhysicalDevice dev = candidates[ i ];

        bool opt[ VK_OPT_EXT_COUNT ] = { 0 };
        u32  gfx, pres, xfer;
        int  score = vk_device_score( dev, &gfx, &pres, &xfer, opt );

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties( dev, &props );

        /* Log full device identity so developers can see exactly what was considered
           and why a particular GPU won or was rejected (score == -1). */
        LOG_INFO( "  [%u] %s type=%s api=%u.%u score=%d", i, 
                  props.deviceName, string_VkPhysicalDeviceType( props.deviceType ),
                  VK_VERSION_MAJOR( props.apiVersion ), VK_VERSION_MINOR( props.apiVersion ),
                  score );

        if ( score > best_score )
        {
            best       = dev;
            best_score = score;
            best_gfx   = gfx;
            best_pres  = pres;
            best_xfer  = xfer;
            best_props = props;
            memcpy( best_opt, opt, sizeof( best_opt ) );
        }
    }

    if ( best == VK_NULL_HANDLE )
    {
        LOG_ERROR( "no suitable physical device "
                   "(requires VK_KHR_swapchain, samplerAnisotropy, fillModeNonSolid, "
                   "graphics+compute queue, present support)" );
        return false;
    }

    vk.physical_device       = best;
    vk.graphics_queue_family = best_gfx;
    vk.present_queue_family  = best_pres;
    vk.transfer_queue_family = best_xfer;
    vk.physical_device_props = best_props;
    memcpy( out_opt_found, best_opt, sizeof( best_opt ) );

    /* Memory properties have no scoring-time equivalent; one call on the winner. */
    vkGetPhysicalDeviceMemoryProperties( vk.physical_device, &vk.memory_props );

    LOG_INFO( "selected: %s  (gfx=%u  present=%u  xfer=%u)",
              vk.physical_device_props.deviceName,
              vk.graphics_queue_family, vk.present_queue_family, vk.transfer_queue_family );

    return true;
}

/*==============================================================================================
    Selected device diagnostics  --  called after vk_device_select; reads from vk.*
==============================================================================================*/

/* Cache key limits in vk.* for runtime use by RHI subsystems, then log them. */
static void
vk_device_cache_limits( void )
{
    const VkPhysicalDeviceLimits* L = &vk.physical_device_props.limits;

    /* Resolve max MSAA to the highest single flag bit supported by both color and depth.
       Stored as VkSampleCountFlagBits so callers can pass it directly to Vulkan APIs. */
    VkSampleCountFlags    msaa_mask = L->framebufferColorSampleCounts
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

/* Logged at TRACE because it is verbose; invaluable when debugging allocation failures
   or choosing the correct memory type index in vk_memory.c. */
static void
vk_device_log_memory( void )
{
    const VkPhysicalDeviceMemoryProperties* mp = &vk.memory_props;

    LOG_TRACE( "memory heaps (%u):", mp->memoryHeapCount );
    for ( u32 i = 0; i < mp->memoryHeapCount; ++i )
    {
        VkMemoryHeapFlags f  = mp->memoryHeaps[ i ].flags;
        double            gb = (double)mp->memoryHeaps[ i ].size / ( 1024.0 * 1024.0 * 1024.0 );

        LOG_TRACE( "  heap[%u]: %.2f GB  %s%s",
                   i, gb,
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
        int  pos        = 0;
        if ( f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT     ) pos += snprintf( buf+pos, 256-pos, "DEVICE_LOCAL "     );
        if ( f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT     ) pos += snprintf( buf+pos, 256-pos, "HOST_VISIBLE "     );
        if ( f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT    ) pos += snprintf( buf+pos, 256-pos, "HOST_COHERENT "    );
        if ( f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT      ) pos += snprintf( buf+pos, 256-pos, "HOST_CACHED "      );
        if ( f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ) pos += snprintf( buf+pos, 256-pos, "LAZILY_ALLOCATED " );
        if ( f & VK_MEMORY_PROPERTY_PROTECTED_BIT        ) pos += snprintf( buf+pos, 256-pos, "PROTECTED "        );

        LOG_TRACE( "  type[%2u] heap=%u : %s", i, h, buf );
    }
}

/*==============================================================================================
    Logical device helpers
==============================================================================================*/

/* All pNext-linked feature structs together in one allocation so the chain can be built
   by a helper without the pointers escaping its stack frame.  feats2 is the chain root. */

typedef struct
{
    VkPhysicalDeviceDescriptorIndexingFeatures  desc_idx;   /* bindless indexing: VK 1.2 */
    VkPhysicalDeviceDynamicRenderingFeatures    dyn_rend;  /* renderpass-free: VK 1.3   */
    VkPhysicalDeviceSynchronization2Features    sync2;     /* barrier2/submit2: VK 1.3  */
    VkPhysicalDeviceTimelineSemaphoreFeatures   timeline;  /* monotonic counter: VK 1.2 */
    VkPhysicalDeviceFeatures2                   feats2;    /* chain root + VK 1.0 feats */

} vk_feature_chain_t;

/* Fill f and wire the pNext links.  Chain order bottom to top:
   desc_idx <- dyn_rend <- sync2 <- timeline <- feats2.
   Only what the RHI uses is opted in; everything else stays VK_FALSE. */
static void
vk_device_init_features( vk_feature_chain_t* f )
{
    f->desc_idx.sType                                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    f->desc_idx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;  /* per-lane dynamic index into bindless array */
    f->desc_idx.descriptorBindingPartiallyBound           = VK_TRUE;  /* bindless slots may be empty/unused        */
    f->desc_idx.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;  /* write new slots while GPU reads others    */
    f->desc_idx.runtimeDescriptorArray                    = VK_TRUE;  /* array size set by layout, not SPIR-V      */

    f->dyn_rend.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    f->dyn_rend.pNext            = &f->desc_idx;
    f->dyn_rend.dynamicRendering = VK_TRUE;  /* vkCmdBeginRendering -- no VkRenderPass / VkFramebuffer */

    f->sync2.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    f->sync2.pNext            = &f->dyn_rend;
    f->sync2.synchronization2 = VK_TRUE;  /* vkCmdPipelineBarrier2 + vkQueueSubmit2 */

    f->timeline.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    f->timeline.pNext             = &f->sync2;
    f->timeline.timelineSemaphore = VK_TRUE;  /* monotonically increasing GPU/CPU sync counter */

    f->feats2.sType                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f->feats2.pNext                      = &f->timeline;
    f->feats2.features.samplerAnisotropy = VK_TRUE;  /* anisotropic texture filtering */
    f->feats2.features.fillModeNonSolid  = VK_TRUE;  /* VK_POLYGON_MODE_LINE wireframe */
}

/* Build the extension list for VkDeviceCreateInfo.  Prepends the required swapchain
   extension, then appends each optional extension reported present by opt_found[].
   out_exts must have room for 1 + VK_OPT_EXT_COUNT entries.  Returns total count. */
static u32
vk_device_collect_extensions( const bool* opt_found, const char** out_exts )
{
    u32 count        = 0;
    out_exts[ count++ ] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;   /* hard requirement */

    for ( u32 i = 0; i < VK_OPT_EXT_COUNT; ++i )
    {
        LOG_INFO( "  optional ext: %-42s  [%s]",
                  s_optional_exts[ i ],
                  opt_found[ i ] ? "enabled" : "not present" );

        if ( opt_found[ i ] )
            out_exts[ count++ ] = s_optional_exts[ i ];
    }

    return count;
}

/* Build deduplicated VkDeviceQueueCreateInfo entries for the three queue families.
   Graphics and present share one family on most desktop hardware; transfer may or may
   not be dedicated.  Duplicate indices are collapsed -- passing the same index twice
   is a validation error.  Returns the number of unique entries written to out_cis. */
static u32
vk_device_build_queue_infos( VkDeviceQueueCreateInfo out_cis[3] )
{
    u32          families[3] = { vk.graphics_queue_family,
                                  vk.present_queue_family,
                                  vk.transfer_queue_family };
    const float* prios[3]    = { &s_prio_high, &s_prio_high, &s_prio_low };

    u32 count = 0;

    for ( u32 i = 0; i < 3; ++i )
    {
        u32 fam = families[ i ];

        bool dup = false;
        for ( u32 j = 0; j < count; ++j )
        {
            if ( out_cis[ j ].queueFamilyIndex == fam ) { dup = true; break; }
        }
        if ( dup ) continue;

        out_cis[ count ]                  = ( VkDeviceQueueCreateInfo ){ 0 };
        out_cis[ count ].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        out_cis[ count ].queueFamilyIndex = fam;
        out_cis[ count ].queueCount       = 1;
        out_cis[ count ].pQueuePriorities = prios[ i ];
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
    /* Phase 1: enumerate GPUs, score, and pick the best candidate.
       opt_found[] is filled during scoring; no second enumerate needed. */

    bool opt_found[ VK_OPT_EXT_COUNT ] = { 0 };
    if ( !vk_device_select( opt_found ) )
        return false;

    /* Phase 2: cache hardware limits in vk.* and log them. */
    vk_device_cache_limits();
    vk_device_log_memory();

    /* Phase 3: build extension list from opt_found; record capability flags. */
    const char* dev_exts[ 1 + VK_OPT_EXT_COUNT ];
    u32 dev_ext_count = vk_device_collect_extensions( opt_found, dev_exts );
    vk.has_push_descriptor = opt_found[ 0 ];   /* s_optional_exts[0] = VK_KHR_push_descriptor */

    /* Phase 4: fill required feature pNext chain. */
    vk_feature_chain_t features = { 0 };
    vk_device_init_features( &features );

    /* Phase 5: build deduplicated queue create infos. */
    VkDeviceQueueCreateInfo qcis[ 3 ];
    u32 qci_count = vk_device_build_queue_infos( qcis );

    /* Phase 6: create the logical device.
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

    LOG_INFO( "device_create: OK" );
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
