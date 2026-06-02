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

==============================================================================================*/
#ifdef OS_WINDOWS
    #pragma warning( disable : 4191 )
#endif

#if !defined( VK_EXPORTED_FUNCTION )
    #define VK_EXPORTED_FUNCTION( fun )
#endif

VK_EXPORTED_FUNCTION( vkGetInstanceProcAddr ) /* root function for getting vk pointers */
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

// Instance
VK_INSTANCE_LEVEL_FUNCTION( vkDestroyInstance )

// Physical Device
VK_INSTANCE_LEVEL_FUNCTION( vkEnumeratePhysicalDevices )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceFeatures )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceQueueFamilyProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceMemoryProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateDevice )

VK_INSTANCE_LEVEL_FUNCTION( vkGetDeviceProcAddr )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfaceSupportKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfaceCapabilitiesKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfaceFormatsKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceSurfacePresentModesKHR )

#if defined( VK_USE_PLATFORM_WIN32_KHR )
VK_INSTANCE_LEVEL_FUNCTION( vkCreateWin32SurfaceKHR )
#endif

VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceWin32PresentationSupportKHR )
VK_INSTANCE_LEVEL_FUNCTION( vkDestroySurfaceKHR )

VK_INSTANCE_LEVEL_FUNCTION( vkEnumerateDeviceExtensionProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkEnumerateDeviceLayerProperties )

VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceFormatProperties )
VK_INSTANCE_LEVEL_FUNCTION( vkGetPhysicalDeviceImageFormatProperties )

#undef VK_INSTANCE_LEVEL_FUNCTION

/*==========================================================================================*/

#if !defined( VK_DEVICE_LEVEL_FUNCTION )
    #define VK_DEVICE_LEVEL_FUNCTION( fun )
#endif

// Device
VK_DEVICE_LEVEL_FUNCTION( vkDestroyDevice )
VK_DEVICE_LEVEL_FUNCTION( vkGetDeviceMemoryCommitment )

// KHR Swapchain
VK_DEVICE_LEVEL_FUNCTION( vkCreateSwapchainKHR )
VK_DEVICE_LEVEL_FUNCTION( vkDestroySwapchainKHR )
VK_DEVICE_LEVEL_FUNCTION( vkGetSwapchainImagesKHR )
VK_DEVICE_LEVEL_FUNCTION( vkAcquireNextImageKHR )
VK_DEVICE_LEVEL_FUNCTION( vkQueuePresentKHR )

// Synchronization
VK_DEVICE_LEVEL_FUNCTION( vkCreateSemaphore )
VK_DEVICE_LEVEL_FUNCTION( vkDestroySemaphore )
VK_DEVICE_LEVEL_FUNCTION( vkCreateFence )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyFence )
VK_DEVICE_LEVEL_FUNCTION( vkWaitForFences )
VK_DEVICE_LEVEL_FUNCTION( vkResetFences )
VK_DEVICE_LEVEL_FUNCTION( vkGetFenceStatus )

VK_DEVICE_LEVEL_FUNCTION( vkDeviceWaitIdle )

// Shader
VK_DEVICE_LEVEL_FUNCTION( vkCreateShaderModule )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyShaderModule )

// Queue
VK_DEVICE_LEVEL_FUNCTION( vkGetDeviceQueue )
VK_DEVICE_LEVEL_FUNCTION( vkQueueSubmit )

// Image
VK_DEVICE_LEVEL_FUNCTION( vkCreateImageView )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyImageView )

// RenderPass
VK_DEVICE_LEVEL_FUNCTION( vkCreateRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBeginRenderPass )
VK_DEVICE_LEVEL_FUNCTION( vkCmdEndRenderPass )

VK_DEVICE_LEVEL_FUNCTION( vkCreateFramebuffer )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyFramebuffer )

// Pipeline
VK_DEVICE_LEVEL_FUNCTION( vkCreatePipelineLayout )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyPipelineLayout )
VK_DEVICE_LEVEL_FUNCTION( vkCreateGraphicsPipelines )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyPipeline )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindPipeline )


// VK_DEVICE_LEVEL_FUNCTION( vkCmdPipelineBarrier )

// Command
VK_DEVICE_LEVEL_FUNCTION( vkCreateCommandPool )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyCommandPool )
VK_DEVICE_LEVEL_FUNCTION( vkResetCommandPool )

VK_DEVICE_LEVEL_FUNCTION( vkAllocateCommandBuffers )
VK_DEVICE_LEVEL_FUNCTION( vkFreeCommandBuffers )
VK_DEVICE_LEVEL_FUNCTION( vkResetCommandBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkBeginCommandBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkEndCommandBuffer )


VK_DEVICE_LEVEL_FUNCTION( vkQueueWaitIdle )

// Draw
VK_DEVICE_LEVEL_FUNCTION( vkCmdBlitImage )
VK_DEVICE_LEVEL_FUNCTION( vkCmdDraw )
VK_DEVICE_LEVEL_FUNCTION( vkCmdDrawIndexed )

// Allocate
VK_DEVICE_LEVEL_FUNCTION( vkAllocateMemory )
VK_DEVICE_LEVEL_FUNCTION( vkFreeMemory )
VK_DEVICE_LEVEL_FUNCTION( vkMapMemory )
VK_DEVICE_LEVEL_FUNCTION( vkUnmapMemory )
VK_DEVICE_LEVEL_FUNCTION( vkFlushMappedMemoryRanges )

// Buffers
VK_DEVICE_LEVEL_FUNCTION( vkCreateBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkCmdCopyBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkGetBufferMemoryRequirements )
VK_DEVICE_LEVEL_FUNCTION( vkBindBufferMemory )

// VK_DEVICE_LEVEL_FUNCTION( vkCmdClearColorImage )

// Commands
VK_DEVICE_LEVEL_FUNCTION( vkCmdSetViewport )
VK_DEVICE_LEVEL_FUNCTION( vkCmdSetScissor )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindVertexBuffers )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindIndexBuffer )
VK_DEVICE_LEVEL_FUNCTION( vkCmdClearColorImage )

// Image
VK_DEVICE_LEVEL_FUNCTION( vkCreateImage )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyImage )
VK_DEVICE_LEVEL_FUNCTION( vkGetImageMemoryRequirements )
VK_DEVICE_LEVEL_FUNCTION( vkBindImageMemory )
VK_DEVICE_LEVEL_FUNCTION( vkGetImageSubresourceLayout )
VK_DEVICE_LEVEL_FUNCTION( vkCmdCopyBufferToImage )
VK_DEVICE_LEVEL_FUNCTION( vkCmdPipelineBarrier )

// Sampler
VK_DEVICE_LEVEL_FUNCTION( vkCreateSampler )
VK_DEVICE_LEVEL_FUNCTION( vkDestroySampler )

// Descriptor
VK_DEVICE_LEVEL_FUNCTION( vkCreateDescriptorSetLayout )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyDescriptorSetLayout )
VK_DEVICE_LEVEL_FUNCTION( vkCreateDescriptorPool )
VK_DEVICE_LEVEL_FUNCTION( vkDestroyDescriptorPool )
VK_DEVICE_LEVEL_FUNCTION( vkAllocateDescriptorSets )
VK_DEVICE_LEVEL_FUNCTION( vkUpdateDescriptorSets )
VK_DEVICE_LEVEL_FUNCTION( vkCmdBindDescriptorSets )
VK_DEVICE_LEVEL_FUNCTION( vkCmdPushConstants )

// VK_KHR_push_descriptor
VK_DEVICE_LEVEL_FUNCTION( vkCmdPushDescriptorSetKHR )

#undef VK_DEVICE_LEVEL_FUNCTION

/*============================================================================================*/