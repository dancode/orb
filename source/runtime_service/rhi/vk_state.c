/*==============================================================================================

    vulkan/vk_state.c — Singleton Vulkan state held by the RHI.

    Included FIRST by rhi.c so every other vk_*.c file sees g_vk through the
    unity TU. Holds all Vulkan handles owned by the RHI for its entire lifetime.

    Initialization is two-phase:
        - rhi_mod_init  (cheap, called by module system at mod_init_all time)
        - rhi()->init( hwnd )  (real, called by host after window exists)

    The split exists because Vulkan surface creation needs a window handle, and
    we want the module system load to remain side-effect-free.

==============================================================================================*/

/* windows.h is already included by rhi.c before this file. */
/* TODO: when implementing, also include <vulkan/vulkan.h> and the platform
         surface extension header (e.g. <vulkan/vulkan_win32.h>) above this. */

/*==============================================================================================
    State
==============================================================================================*/

#define VK_MAX_FRAMES_IN_FLIGHT 2

typedef struct vk_state_s
{
    bool  initialized;        /* set true at the end of a successful rhi_init() */
    void* native_window;      /* HWND on Windows; cast at use sites */

    i32   width;
    i32   height;
    bool  resize_pending;     /* set when WM_SIZE causes a deferred swap-chain rebuild */

    u32   current_frame;      /* index into per-frame arrays, 0..VK_MAX_FRAMES_IN_FLIGHT-1 */

    /* TODO (Vulkan implementation) — uncomment as each is wired up:

        VkInstance               instance;
        VkDebugUtilsMessengerEXT debug_messenger;

        VkPhysicalDevice         physical_device;
        VkPhysicalDeviceProperties physical_device_props;
        u32                      graphics_queue_family;
        u32                      present_queue_family;
        VkDevice                 device;
        VkQueue                  graphics_queue;
        VkQueue                  present_queue;

        VkSurfaceKHR             surface;
        VkSurfaceFormatKHR       surface_format;
        VkPresentModeKHR         present_mode;
        VkSwapchainKHR           swapchain;
        u32                      swapchain_image_count;
        VkImage                  swapchain_images[8];
        VkImageView              swapchain_image_views[8];
        VkExtent2D               swapchain_extent;

        VkCommandPool            command_pool;
        VkCommandBuffer          command_buffers   [VK_MAX_FRAMES_IN_FLIGHT];

        VkSemaphore              image_available_sem[VK_MAX_FRAMES_IN_FLIGHT];
        VkSemaphore              render_finished_sem[VK_MAX_FRAMES_IN_FLIGHT];
        VkFence                  in_flight_fence    [VK_MAX_FRAMES_IN_FLIGHT];
    */

} vk_state_t;

static vk_state_t g_vk;

/*============================================================================================*/
