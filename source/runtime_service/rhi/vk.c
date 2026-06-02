/*==============================================================================================
    vk.c  -- Unity build entry point for the Vulkan RHI implementation.
==============================================================================================*/

#define VK_USE_PLATFORM_WIN32_KHR    // exposes platform functions in vulkan.h
#define VK_NO_PROTOTYPES             // we dynamically link our own api function pointers.

/* we have to guard the vulkan included to prevent warnigs with /wall */

PUSH_WARNINGS
// #include <vulkan/vulkan.h>
// #include <vulkan/vk_enum_string_helper.h>
POP_WARNINGS

/*============================================================================================*/