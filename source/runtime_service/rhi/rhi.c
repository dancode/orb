/*==============================================================================================

    runtime/services/rhi/rhi.c -- Unity build entry point for the RHI.

    Mirrors sys.c / core.c / app.c.  The build system compiles only this file for
    rhi; every backend file and the API wiring are #included here.

    Inclusion order matters
    -----------------------
        1. Standard headers
        2. orb.h  +  LOG_CH  +  engine headers
        3. Platform headers        (windows.h, gated by OS_WINDOWS)
        4. Vulkan headers          (platform-gated; VK_NO_PROTOTYPES so we own all pointers)
        5. rhi_api.h               (RHI API struct + gateway; transitively includes rhi.h)
        6. vk_state.c              (vk singleton and type definitions -- MUST be first)
        7. vk_library.c            (Vulkan DLL load + function pointer bootstrap)
        8. vk_debug.c              (allocation callbacks, validation messenger, GPU labels)
        9. vk_convert.c            (all RHI -> Vulkan enum/format translations)
       10. vk_memory.c             (device memory allocation)
       11. vk_texture.c            (VkImage + VkImageView + VkSampler lifecycle)
       12. vk_buffer.c             (VkBuffer lifecycle)
       13. vk_shader.c             (VkShaderModule lifecycle)
       14. vk_descriptor.c         (bindless pool + shared pipeline layout)
       15. vk_pipeline_cache.c     (VkPipelineCache disk persistence)
       16. vk_pipeline_graphics.c  (slot helpers + graphics PSO creation)
       17. vk_pipeline_compute.c   (compute PSO creation)
       18. vk_upload.c             (staged upload ring buffer + QFOT)
       19. vk_instance.c           (VkInstance + extensions + layers)
       20. vk_swapchain.c          (VkSurfaceKHR + VkSwapchainKHR + depth buffer)
       21. vk_device_select.c      (physical device enumeration + feature validation)
       22. vk_device.c             (VkDevice creation + queue retrieval + subsystem init)
       23. vk_sync.c               (per-frame semaphores + fences)
       24. vk_command.c            (command pool + per-frame command buffers)
       25. vk_frame.c              (frame_begin / frame_end orchestration)
       26. vk_cmd_graphics.c       (render pass, draw calls, state binding)
       27. vk_cmd_compute.c        (compute dispatch)
       28. vk_init.c               (global and per-context lifecycle)
       29. rhi_api.c               (wires vk_* functions into API struct + mod descriptor)

==============================================================================================*/

/*==============================================================================================
    Standard headers
==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "orb.h"

#define LOG_CH "vk"                     // for core log macros in vk_* files

#include "engine/sys/sys_host.h"        // load_library, get_proc_address
#include "engine/core/core_host.h"      // log and assert

/*==============================================================================================
    Platform Headers
==============================================================================================*/

#if OS_WINDOWS

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN
    #include <windows.h>

#elif OS_LINUX

    /* Xlib or Wayland depending on build configuration.
       Define VK_USE_PLATFORM_WAYLAND_KHR before including vulkan.h to select Wayland.
       Default is Xlib. */
    #if !defined( VK_USE_PLATFORM_WAYLAND_KHR )
        #define VK_USE_PLATFORM_XLIB_KHR
    #endif

#elif OS_MAC

    /* MoltenVK exposes Vulkan via a Metal layer. */

#endif

/*==============================================================================================
    Vulkan headers  (VK_NO_PROTOTYPES: we own all function pointer declarations)
==============================================================================================*/

/* NOTE: We are Win32 only for Vulkan currently!!! */

#if OS_WINDOWS

    #define VK_USE_PLATFORM_WIN32_KHR
    #define VK_NO_PROTOTYPES
    #include <vulkan/vulkan.h>
    #include <vulkan/vulkan_win32.h>
    #include <vulkan/vk_enum_string_helper.h>

#elif OS_LINUX

    #define VK_NO_PROTOTYPES
    #include <vulkan/vulkan.h>
    #if defined( VK_USE_PLATFORM_WAYLAND_KHR )
        #include <vulkan/vulkan_wayland.h>
    #else
        #include <vulkan/vulkan_xlib.h>
    #endif

#elif OS_MAC

    #define VK_USE_PLATFORM_METAL_EXT
    #define VK_NO_PROTOTYPES
    #include <vulkan/vulkan.h>
    #include <vulkan/vulkan_metal.h>

#endif

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "runtime_service/rhi/rhi_api.h"

/*==============================================================================================
    Vulkan backend  (vk_state.c FIRST; every other file depends on it)
==============================================================================================*/

#include "runtime_service/rhi/vk_state.c"
#include "runtime_service/rhi/vk_library.c"
#include "runtime_service/rhi/vk_debug.c"
#include "runtime_service/rhi/vk_convert.c"

#include "runtime_service/rhi/vk_memory.c"
#include "runtime_service/rhi/vk_texture.c"
#include "runtime_service/rhi/vk_buffer.c"
#include "runtime_service/rhi/vk_shader.c"
#include "runtime_service/rhi/vk_descriptor.c"
#include "runtime_service/rhi/vk_pipeline_cache.c"
#include "runtime_service/rhi/vk_pipeline_graphics.c"
#include "runtime_service/rhi/vk_pipeline_compute.c"
#include "runtime_service/rhi/vk_upload.c"

static void vk_device_wait_idle( void );

#include "runtime_service/rhi/vk_instance.c"
#include "runtime_service/rhi/vk_swapchain.c"
#include "runtime_service/rhi/vk_device_select.c"
#include "runtime_service/rhi/vk_device.c"
#include "runtime_service/rhi/vk_sync.c"
#include "runtime_service/rhi/vk_command.c"
#include "runtime_service/rhi/vk_frame.c"
#include "runtime_service/rhi/vk_cmd_graphics.c"
#include "runtime_service/rhi/vk_cmd_compute.c"

#include "runtime_service/rhi/vk_init.c"

/*==============================================================================================
    API wiring + module descriptor  (must be last)
==============================================================================================*/

#ifndef RHI_API_C_PRELUDE
    #include "runtime_service/rhi/rhi_api.c"
#endif

/*============================================================================================*/
