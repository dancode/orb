/*==============================================================================================

    vulkan/vk_functions.h -- X-macro table of every Vulkan function pointer used by the RHI.

    Include this file up to four times, each time with one VK_*_FUNCTION macro defined.
    Undefined macros expand to nothing so a single inclusion safely skips other levels.
    Each section #undefs its macro after its entries so the next inclusion starts clean.

    Load levels (must be resolved in order):
      VK_EXPORTED_FUNCTION       -- exported from vulkan-1.dll; retrieved via GetProcAddress
      VK_GLOBAL_LEVEL_FUNCTION   -- vkGetInstanceProcAddr( NULL, ... ); pre-instance helpers
      VK_INSTANCE_LEVEL_FUNCTION -- vkGetInstanceProcAddr( instance, ... ); post-CreateInstance
      VK_DEVICE_LEVEL_FUNCTION   -- vkGetDeviceProcAddr( device, ... ); post-CreateDevice

    VK 1.3 target: dynamic rendering + synchronization2 + descriptor indexing are core.
    Extension functions (push descriptors, debug utils) follow the same load path because
    the loader exposes them via vkGetInstanceProcAddr / vkGetDeviceProcAddr.

==============================================================================================*/
#ifdef OS_WINDOWS
    #pragma warning( disable : 4191 )
#endif

#if !defined( VK_EXPORTED_FUNCTION )
    #define VK_EXPORTED_FUNCTION( fun )
#endif

VK_EXPORTED_FUNCTION( vkGetInstanceProcAddr )      /* root bootstrap */
VK_EXPORTED_FUNCTION( vkEnumerateInstanceVersion )

#undef VK_EXPORTED_FUNCTION

/*============================================================================================*/

#if !defined( VK_GLOBAL_LEVEL_FUNCTION )
    #define VK_GLOBAL_LEVEL_FUNCTION( fun )
#endif

VK_GLOBAL_LEVEL_FUNCTION( vkEnumerateInstanceExtensionProperties )
VK_GLOBAL_LEVEL_FUNCTION( vkEnumerateInstanceLayerProperties )
VK_GLOBAL_LEVEL_FUNCTION( vkCreateInstance )

#undef VK_GLOBAL_LEVEL_FUNCTION

/*============================================================================================*/

#if !defined( VK_INSTANCE_LEVEL_FUNCTION )
    #define VK_INSTANCE_LEVEL_FUNCTION( fun )
#endif

/* Instance */
VK_INSTANCE_LEVEL_FUNCTION( vkDestroyInstance )

/* Physical device */
VK_INSTANCE_LEVEL_FUNCTION( vkEnumeratePhysicalDevices )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceProperties2 )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceFeatures )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceFeatures2 )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceQueueFamilyProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceMemoryProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceMemoryProperties2 )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateDevice )
VK_INSTANCE_LEVEL_FUNCTION( vkGetDeviceProcAddr )

/* Surface */
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfaceSupportKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfaceCapabilitiesKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfaceFormatsKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfacePresentModesKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkDestroySurfaceKHR )

/* Platform surfaces (guarded; only declared if platform header was included) */
#if defined( VK_USE_PLATFORM_WIN32_KHR )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateWin32SurfaceKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceWin32PresentationSupportKHR )
#endif

#if defined( VK_USE_PLATFORM_XLIB_KHR )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateXlibSurfaceKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceXlibPresentationSupportKHR )
#endif

#if defined( VK_USE_PLATFORM_WAYLAND_KHR )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateWaylandSurfaceKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceWaylandPresentationSupportKHR )
#endif

#if defined( VK_USE_PLATFORM_METAL_EXT )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateMetalSurfaceEXT )
#endif

/* Device extensions enumeration */
VK_INSTANCE_LEVEL_FUNCTION( vkEnumerateDeviceExtensionProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkEnumerateDeviceLayerProperties )

/* Format queries */
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceFormatProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceImageFormatProperties )

/* Debug utils (VK_EXT_debug_utils; debug builds only, but loaded unconditionally) */
VK_INSTANCE_LEVEL_FUNCTION( vkCreateDebugUtilsMessengerEXT )
VK_INSTANCE_LEVEL_FUNCTION( vkDestroyDebugUtilsMessengerEXT )

#undef VK_INSTANCE_LEVEL_FUNCTION

/*==========================================================================================*/

#if !defined( VK_DEVICE_LEVEL_FUNCTION )
    #define VK_DEVICE_LEVEL_FUNCTION( fun )
#endif

/* Device */
VK_DEVICE_LEVEL_FUNCTION( vkDestroyDevice )
VK_DEVICE_LEVEL_FUNCTION( vkDeviceWaitIdle )
VK_DEVICE_LEVEL_FUNCTION( vkGetDeviceMemoryCommitment )

/* Queue */
VK_DEVICE_LEVEL_FUNCTION( vkGetDeviceQueue )
VK_DEVICE_LEVEL_FUNCTION( vkQueueWaitIdle )
VK_DEVICE_LEVEL_FUNCTION( vkQueueSubmit )

/* Synchronization 2 (core VK 1.3) -- preferred path */
VK_DEVICE_LEVEL_FUNCTION( vkQueueSubmit2 )

/* Fences */
VK_DEVICE_LEVEL_FUNCTION( vkCreateFence )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyFence )
VK_DEVICE_LEVEL_FUNCTION( vkWaitForFences )
VK_DEVICE_LEVEL_FUNCTION( vkResetFences )
VK_DEVICE_LEVEL_FUNCTION( vkGetFenceStatus )

/* Semaphores (binary + timeline; timeline is core VK 1.2) */
VK_DEVICE_LEVEL_FUNCTION( vkCreateSemaphore )
VK_DEVICE_LEVEL_FUNCTION( vkDestroySemaphore )
VK_DEVICE_LEVEL_FUNCTION( vkWaitSemaphores )
VK_DEVICE_LEVEL_FUNCTION( vkSignalSemaphore )
VK_DEVICE_LEVEL_FUNCTION( vkGetSemaphoreCounterValue )

/* Command pools and buffers */
VK_DEVICE_LEVEL_FUNCTION( vkCreateCommandPool )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyCommandPool )
VK_DEVICE_LEVEL_FUNCTION( vkResetCommandPool )
VK_DEVICE_LEVEL_FUNCTION( vkAllocateCommandBuffers )
VK_DEVICE_LEVEL_FUNCTION( vkFreeCommandBuffers )
VK_DEVICE_LEVEL_FUNCTION( vkResetCommandBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkBeginCommandBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkEndCommandBuffer )

/* Dynamic rendering (core VK 1.3) -- replaces VkRenderPass / VkFramebuffer */
VK_DEVICE_LEVEL_FUNCTION( vkCmdBeginRendering )
VK_DEVICE_LEVEL_FUNCTION( vkCmdEndRendering )

/* Render pass (kept for ImGui / compatibility paths that require it) */
VK_DEVICE_LEVEL_FUNCTION( vkCreateRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBeginRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkCmdEndRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkCreateFramebuffer )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyFramebuffer )

/* Pipeline barriers -- legacy (VK 1.0) and synchronization2 (VK 1.3) */
VK_DEVICE_LEVEL_FUNCTION( vkCmdPipelineBarrier )
VK_DEVICE_LEVEL_FUNCTION( vkCmdPipelineBarrier2 )

/* Draw */
VK_DEVICE_LEVEL_FUNCTION( vkCmdDraw )
VK_DEVICE_LEVEL_FUNCTION( vkCmdDrawIndexed )
VK_DEVICE_LEVEL_FUNCTION( vkCmdDrawIndirect )
VK_DEVICE_LEVEL_FUNCTION( vkCmdDrawIndexedIndirect )
VK_DEVICE_LEVEL_FUNCTION( vkCmdDispatch )

/* State */
VK_DEVICE_LEVEL_FUNCTION( vkCmdSetViewport )
VK_DEVICE_LEVEL_FUNCTION( vkCmdSetScissor )

/* Pipeline */
VK_DEVICE_LEVEL_FUNCTION( vkCreatePipelineLayout )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyPipelineLayout )
VK_DEVICE_LEVEL_FUNCTION( vkCreateGraphicsPipelines )
VK_DEVICE_LEVEL_FUNCTION( vkCreateComputePipelines )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyPipeline )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindPipeline )
VK_DEVICE_LEVEL_FUNCTION( vkCreatePipelineCache )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyPipelineCache )
VK_DEVICE_LEVEL_FUNCTION( vkGetPipelineCacheData )
VK_DEVICE_LEVEL_FUNCTION( vkMergePipelineCaches )

/* Shaders */
VK_DEVICE_LEVEL_FUNCTION( vkCreateShaderModule )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyShaderModule )

/* Vertex / index binding */
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindVertexBuffers )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindIndexBuffer )

/* Push constants */
VK_DEVICE_LEVEL_FUNCTION( vkCmdPushConstants )

/* Blit / clear */
VK_DEVICE_LEVEL_FUNCTION( vkCmdBlitImage )
VK_DEVICE_LEVEL_FUNCTION( vkCmdClearColorImage )

/* Memory */
VK_DEVICE_LEVEL_FUNCTION( vkAllocateMemory )
VK_DEVICE_LEVEL_FUNCTION( vkFreeMemory )
VK_DEVICE_LEVEL_FUNCTION( vkMapMemory )
VK_DEVICE_LEVEL_FUNCTION( vkUnmapMemory )
VK_DEVICE_LEVEL_FUNCTION( vkFlushMappedMemoryRanges )
VK_DEVICE_LEVEL_FUNCTION( vkInvalidateMappedMemoryRanges )

/* Buffers */
VK_DEVICE_LEVEL_FUNCTION( vkCreateBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkGetBufferMemoryRequirements )
VK_DEVICE_LEVEL_FUNCTION( vkGetBufferMemoryRequirements2 )
VK_DEVICE_LEVEL_FUNCTION( vkBindBufferMemory )
VK_DEVICE_LEVEL_FUNCTION( vkCmdCopyBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkGetBufferDeviceAddress )   /* VK 1.2: returns 64-bit GPU VA for BDA buffers */

/* Images */
VK_DEVICE_LEVEL_FUNCTION( vkCreateImage )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyImage )
VK_DEVICE_LEVEL_FUNCTION( vkGetImageMemoryRequirements )
VK_DEVICE_LEVEL_FUNCTION( vkGetImageMemoryRequirements2 )
VK_DEVICE_LEVEL_FUNCTION( vkBindImageMemory )
VK_DEVICE_LEVEL_FUNCTION( vkCreateImageView )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyImageView )
VK_DEVICE_LEVEL_FUNCTION( vkGetImageSubresourceLayout )
VK_DEVICE_LEVEL_FUNCTION( vkCmdCopyBufferToImage )
VK_DEVICE_LEVEL_FUNCTION( vkCmdCopyImageToBuffer )

/* Samplers */
VK_DEVICE_LEVEL_FUNCTION( vkCreateSampler )
VK_DEVICE_LEVEL_FUNCTION( vkDestroySampler )

/* Descriptors */
VK_DEVICE_LEVEL_FUNCTION( vkCreateDescriptorSetLayout )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyDescriptorSetLayout )
VK_DEVICE_LEVEL_FUNCTION( vkCreateDescriptorPool )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyDescriptorPool )
VK_DEVICE_LEVEL_FUNCTION( vkResetDescriptorPool )
VK_DEVICE_LEVEL_FUNCTION( vkAllocateDescriptorSets )
VK_DEVICE_LEVEL_FUNCTION( vkUpdateDescriptorSets )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindDescriptorSets )

/* Push descriptors (VK_KHR_push_descriptor; check device caps before use) */
VK_DEVICE_LEVEL_FUNCTION( vkCmdPushDescriptorSetKHR )

/* Swapchain */
VK_DEVICE_LEVEL_FUNCTION( vkCreateSwapchainKHR )
VK_DEVICE_LEVEL_FUNCTION( vkDestroySwapchainKHR )
VK_DEVICE_LEVEL_FUNCTION( vkGetSwapchainImagesKHR )
VK_DEVICE_LEVEL_FUNCTION( vkAcquireNextImageKHR )
VK_DEVICE_LEVEL_FUNCTION( vkQueuePresentKHR )

/* Debug utils -- object naming + GPU labels (VK_EXT_debug_utils) */
VK_DEVICE_LEVEL_FUNCTION( vkSetDebugUtilsObjectNameEXT )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBeginDebugUtilsLabelEXT )
VK_DEVICE_LEVEL_FUNCTION( vkCmdEndDebugUtilsLabelEXT )
VK_DEVICE_LEVEL_FUNCTION( vkCmdInsertDebugUtilsLabelEXT )

#undef VK_DEVICE_LEVEL_FUNCTION

/*============================================================================================*/
