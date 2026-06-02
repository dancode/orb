/*==============================================================================================

    runtime/services/rhi/rhi.c — Unity build entry point for the RHI.

    Mirrors sys.c / core.c / app.c. The CMakeLists compiles only this file for
    rt_rhi; every backend file and the API wiring are #included here.

    Inclusion order matters
    -----------------------
        1. Standard headers
        2. orb.h
        3. Platform headers           (windows.h, gated by OS_WINDOWS)
        4. rhi.h                      (handle types, API struct, gateway)
        5. vk_state.c                 (the singleton — everything else uses it)
        6. vk_*.c                     (subsystem implementations)
        7. vk_init.c                  (orchestrates init/shutdown/resize across subsystems)
        8. rhi_api.c                  (assigns functions into g_rhi_api_struct,
                                       provides the module descriptor)
    Vulkan headers
    --------------
    When implementation begins, <vulkan/vulkan.h> and the platform surface header
    (<vulkan/vulkan_win32.h> on Windows) should be added in the Platform headers
    block below, BEFORE rhi.h. Volk-style loader loading happens in vk_instance.c.

==============================================================================================*/

/*==============================================================================================
    Standard headers
==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"

#define LOG_CH "vk" /* called before core_api_log.h */

#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"

/*==============================================================================================
    Platform headers
==============================================================================================*/

#if OS_WINDOWS

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN
    #include <windows.h>

#else

    #error "rhi: platform not implemented"

#endif

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "runtime_service/rhi/rhi_api.h" /* rhi API struct -- non-vk function names */

/*==============================================================================================
    Vulkan platform headers
==============================================================================================*/

#if OS_WINDOWS

    #define VK_USE_PLATFORM_WIN32_KHR    // exposes platform functions in vulkan.h
    #define VK_NO_PROTOTYPES             // we dynamically link our own api function pointers.

    #include <vulkan/vulkan.h>
    #include <vulkan/vulkan_win32.h>
    #include <vulkan/vk_enum_string_helper.h>

#endif

/*==============================================================================================
    Vulkan backend  (vk_state.c FIRST so g_vk is visible to everything below)
==============================================================================================*/

#include "runtime_service/rhi/vk.c"             /* nothing currently */
#include "runtime_service/rhi/vk_state.c"       /* the singleton Vulkan state struct */
#include "runtime_service/rhi/vk_library.c"     /* Vulkan function pointer loading */

#include "runtime_service/rhi/vk_instance.c"
#include "runtime_service/rhi/vk_swapchain.c"
#include "runtime_service/rhi/vk_device.c"
#include "runtime_service/rhi/vk_sync.c"
#include "runtime_service/rhi/vk_command.c"
#include "runtime_service/rhi/vk_frame.c"

#include "runtime_service/rhi/vk_init.c"        /* setup vulkan RHI */

/* Future files (added as subsystems come online):
       #include "runtime_service/rhi/vk_memory.c"
       #include "runtime_service/rhi/vk_buffer.c"
       #include "runtime_service/rhi/vk_texture.c"
       #include "runtime_service/rhi/vk_pipeline.c"
       #include "runtime_service/rhi/vk_shader.c"
       #include "runtime_service/rhi/vk_descriptor.c"
*/


/*==============================================================================================
    API wiring + lifecycle orchestrator  (must be last)
==============================================================================================*/

#ifndef RHI_API_C_PRELUDE
    #include "runtime_service/rhi/rhi_api.c"
#endif


/*============================================================================================*/
