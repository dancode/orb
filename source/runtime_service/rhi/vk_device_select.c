/*==============================================================================================

    vk_device_select.c -- Physical device enumeration, feature validation, and selection.

    Enumerates all Vulkan-capable devices, scores them by type preference, validates each
    candidate against hard feature requirements, and writes the chosen device into vk.*.
    No Vulkan objects are created here -- this is pure query + selection work.

    Required Vulkan 1.3 features (device rejected if absent):
        dynamicRendering    -- vkCmdBeginRendering; eliminates VkRenderPass objects
        synchronization2    -- vkCmdPipelineBarrier2 / vkQueueSubmit2 (VkDependencyInfo)
        timelineSemaphore   -- monotonic GPU/CPU sync counter; no polling loops needed

    Required Vulkan 1.2 features (device rejected if absent):
        descriptorIndexing:
            shaderSampledImageArrayNonUniformIndexing  -- per-lane bindless array index
            descriptorBindingPartiallyBound            -- bindless slots may be empty
            descriptorBindingSampledImageUpdateAfterBind -- UPDATE_AFTER_BIND on images
            descriptorBindingUpdateUnusedWhilePending  -- stream new slots while GPU reads others
            runtimeDescriptorArray                     -- array size from layout, not SPIR-V
        bufferDeviceAddress -- 64-bit GPU virtual addresses for BDA buffers

    Required VkPhysicalDeviceFeatures (device rejected if absent):
        samplerAnisotropy   -- anisotropic filtering
        fillModeNonSolid    -- VK_POLYGON_MODE_LINE for wireframe overlays

    Optional extensions (checked here; enabled in vk_device.c):
        VK_KHR_push_descriptor  -- inline per-draw descriptor updates

==============================================================================================*/
// clang-format off

#define VK_MAX_PHYSICAL_DEVICES     8
#define VK_MAX_QUEUE_FAMILIES       16
#define VK_QUEUE_FAMILY_INVALID     ( ~0u )

/* Optional extensions: checked against the selected device; enabled only if present.
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
    VkPhysicalDeviceDescriptorIndexingFeatures  desc_idx;  /* bindless indexing: VK 1.2 */
    VkPhysicalDeviceBufferDeviceAddressFeatures bda;       /* GPU buffer pointers: VK 1.2 */
    VkPhysicalDeviceDynamicRenderingFeatures    dyn_rend;  /* renderpass-free: VK 1.3    */
    VkPhysicalDeviceSynchronization2Features    sync2;     /* barrier2/submit2: VK 1.3   */
    VkPhysicalDeviceTimelineSemaphoreFeatures   timeline;  /* monotonic counter: VK 1.2  */
    VkPhysicalDeviceFeatures2                   feats2;    /* chain root + VK 1.0 feats  */

} vk_feature_chain_t;

/*==============================================================================================
    Check required extension + probe which optional extensions are present.

    Extension counts reach 200+ on current drivers so we heap-allocate rather than risk
    a large stack frame.
==============================================================================================*/

static bool
vk_device_validate_extensions( VkPhysicalDevice dev, bool* out_optional )
{
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, NULL );
    VkExtensionProperties* exts = malloc( sizeof( VkExtensionProperties ) * ( count + 1 ) );
    if ( !exts ) return false;
    vkEnumerateDeviceExtensionProperties( dev, NULL, &count, exts );

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
    Get queue family indices for graphics + compute, present, and transfer.

    Combined graphics + compute:
        Render and compute share the same timeline so they can occupy the same command
        buffer without cross-family ownership transfers or extra pipeline barriers.

    Dedicated transfer:
        Discrete AMD and NVIDIA expose a DMA-only queue family that runs in parallel
        with the 3D engine.  Falls back to the graphics family when absent.

    Surface-free present query:
        vkGetPhysicalDeviceSurfaceSupportKHR requires a live VkSurfaceKHR which does not
        exist at device-select time.  The Win32 platform query answers without one.
        context_create re-validates via vkGetPhysicalDeviceSurfaceSupportKHR once a real
        surface is available.

    Returns false if graphics or present cannot be satisfied.
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

        /* Combined graphics + compute: prefer a family that supports both. */
        if ( gfx == VK_QUEUE_FAMILY_INVALID && ( flags & VK_QUEUE_GRAPHICS_BIT ) &&
                                               ( flags & VK_QUEUE_COMPUTE_BIT  )) {
             gfx = i;
        }

        /* Surface-free present support query (platform-specific). */
        if ( present == VK_QUEUE_FAMILY_INVALID )
        {
            #if defined( VK_USE_PLATFORM_WIN32_KHR )
                /* Surface-free query answers without a HWND or VkSurfaceKHR.
                   On most Win32 drivers the graphics family also supports present. */
                if ( vkGetPhysicalDeviceWin32PresentationSupportKHR( dev, i ) )
                    present = i;
            #else
                /* Linux/Mac: no surface-free present query without a live display
                   connection.  Accept any graphics family; context_create confirms
                   via vkGetPhysicalDeviceSurfaceSupportKHR. */
                if ( flags & VK_QUEUE_GRAPHICS_BIT )
                    present = i;
            #endif
        }

        /* Dedicated transfer: TRANSFER set, GRAPHICS and COMPUTE absent. */
        if ( transfer == VK_QUEUE_FAMILY_INVALID &&  ( flags & VK_QUEUE_TRANSFER_BIT ) &&
             !( flags & VK_QUEUE_GRAPHICS_BIT  ) && !( flags & VK_QUEUE_COMPUTE_BIT  ))
        {
            transfer = i;
        }
    }

    /* Fall back to graphics family when no dedicated transfer family exists. */
    if ( transfer == VK_QUEUE_FAMILY_INVALID )
         transfer = gfx;

    *out_gfx      = gfx;
    *out_present  = present;
    *out_transfer = transfer;

    return ( gfx != VK_QUEUE_FAMILY_INVALID ) && ( present != VK_QUEUE_FAMILY_INVALID );
}

/*==============================================================================================
    Full compatibility check for a single device candidate.
==============================================================================================*/

static bool
vk_device_validate( VkPhysicalDevice dev,
                    u32* out_gfx, u32* out_present, u32* out_transfer, bool* out_opt )
{
    if ( !vk_device_validate_extensions( dev, out_opt ))
         return false;

    /* Chain all required 1.0/1.2/1.3 feature structs so vkGetPhysicalDeviceFeatures2
       populates every flag in one call, then reject the device if any requirement fails. */
    vk_feature_chain_t f    = { 0 };
    f.desc_idx.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    f.bda.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    f.bda.pNext             = &f.desc_idx;
    f.dyn_rend.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    f.dyn_rend.pNext        = &f.bda;
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

    /* VK 1.2 buffer device address requirement */
    if ( !f.bda.bufferDeviceAddress ) {
        return false;
    }

    /* VK 1.3 requirements */
    if ( !f.dyn_rend.dynamicRendering ||
         !f.sync2.synchronization2    ||
         !f.timeline.timelineSemaphore ) {
        return false;
    }

    return vk_device_validate_queues( dev, out_gfx, out_present, out_transfer );
}

/*==============================================================================================
    Pick the best compatible device and store the result in vk.*.
==============================================================================================*/

static bool
vk_device_select( bool* out_opt_found )
{
    VkPhysicalDevice            device_array[  VK_MAX_PHYSICAL_DEVICES ];
    VkPhysicalDeviceProperties  device_props[  VK_MAX_PHYSICAL_DEVICES ];
    u32                         device_count = VK_MAX_PHYSICAL_DEVICES;

    VkResult result = vkEnumeratePhysicalDevices( vk.instance, &device_count, device_array );
    if (( result != VK_SUCCESS && result != VK_INCOMPLETE ) || device_count == 0 ) {
        LOG_ERROR( "vkEnumeratePhysicalDevices: %s (count=%u)", string_VkResult( result ), device_count );
        return false;
    }

    LOG_INFO( "physical devices found: %u", device_count );

    for ( u32 i = 0; i < device_count; ++i )
        vkGetPhysicalDeviceProperties( device_array[ i ], &device_props[ i ] );

    for ( u32 i = 0; i < device_count; ++i ) {
        LOG_INFO( "[%u] %s type=%s api=%u.%u", i,
                  device_props[ i ].deviceName, string_VkPhysicalDeviceType( device_props[ i ].deviceType ),
                  VK_VERSION_MAJOR( device_props[ i ].apiVersion ), VK_VERSION_MINOR( device_props[ i ].apiVersion ) );
    }

    /* Validate the first compatible device in order of type preference. */
    static const VkPhysicalDeviceType s_pref[] = {
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,  VK_PHYSICAL_DEVICE_TYPE_CPU,
        VK_PHYSICAL_DEVICE_TYPE_OTHER,
    };

    u32 pref_count = sizeof( s_pref ) / sizeof( s_pref[ 0 ] );
    for ( u32 t = 0; t < pref_count; ++t )
    {
        for ( u32 i = 0; i < device_count; ++i )
        {
            if ( device_props[ i ].deviceType != s_pref[ t ] )
                 continue;

            bool opt[ VK_OPT_EXT_COUNT ] = { 0 };
            u32  gfx, pres, xfer;

            if ( !vk_device_validate( device_array[ i ], &gfx, &pres, &xfer, opt )) {
                 LOG_INFO( "  skipped %s (failed compatibility check)", device_props[ i ].deviceName );
                 continue;
            }

            vk.physical_device       = device_array[ i ];
            vk.physical_device_props = device_props[ i ];
            vk.graphics_queue_family = gfx;
            vk.present_queue_family  = pres;
            vk.transfer_queue_family = xfer;
            vkGetPhysicalDeviceMemoryProperties( vk.physical_device, &vk.memory_props );

            memcpy( out_opt_found, opt, VK_OPT_EXT_COUNT * sizeof( bool ) );

            LOG_INFO( "best match: [%u]", i );
            LOG_INFO( "  device named : %s", vk.physical_device_props.deviceName );
            LOG_INFO( "  device type  : %s", string_VkPhysicalDeviceType( vk.physical_device_props.deviceType ));
            LOG_INFO( "  queue family : Graphics = %u Present = %u Transfer = %u", gfx, pres, xfer );
            LOG_INFO( "  api          : %s", vk_version_string( vk.physical_device_props.apiVersion ));
            LOG_INFO( "  driver       : %s", vk_version_string( vk.physical_device_props.driverVersion ));
            LOG_INFO( "  vendor id    : %u", vk.physical_device_props.vendorID );

            return true;
        }
    }
    LOG_ERROR( "no suitable physical device (requires VK_KHR_swapchain, samplerAnisotropy, "
               "fillModeNonSolid, graphics + compute queue, present support)" );

    return false;
}

/*============================================================================================*/
// clang-format on
