/*==============================================================================================

    vulkan/vk_state.c -- Vulkan state: global singleton + render-context pool.

    Included FIRST by rhi.c so every other vk_*.c file sees g_vk.

    Initialization is three-phase:
        - rhi_mod_init          (cheap: loads the Vulkan DLL via vk_lib_init)
        - rhi()->init()         (global: VkInstance + VkDevice, no window)
        - rhi()->context_create (per-window: surface, swapchain, sync, commands)

==============================================================================================*/
// clang-format off
/*==============================================================================================
    Per-context state  (one per platform window)
==============================================================================================*/

#define VK_MAX_FRAMES_IN_FLIGHT 2

typedef struct vk_context_s
{
    /* identity */
    i32    id;
    i32        win_id;
    void*           native_window;   /* HWND on Windows; cast at use sites */

    /* dimensions */
    i32             width;
    i32             height;
    bool            resize_pending;  /* deferred swapchain rebuild, handled at next frame_begin */

    /* frame tracking */
    u32             current_frame;   /* index into per-frame arrays, 0..VK_MAX_FRAMES_IN_FLIGHT-1 */

    /* TODO (Vulkan implementation) -- uncomment as each is wired up:

    VkSurfaceKHR               surface;
    VkSurfaceFormatKHR         surface_format;
    VkPresentModeKHR           present_mode;
    VkSwapchainKHR             swapchain;
    u32                        swapchain_image_count;
    VkImage                    swapchain_images[ 8 ];
    VkImageView                swapchain_image_views[ 8 ];
    VkExtent2D                 swapchain_extent;

    VkCommandPool              command_pool;
    VkCommandBuffer            command_buffers[ VK_MAX_FRAMES_IN_FLIGHT ];

    VkSemaphore                image_available_sem[ VK_MAX_FRAMES_IN_FLIGHT ];
    VkSemaphore                render_finished_sem[ VK_MAX_FRAMES_IN_FLIGHT ];
    VkFence                    in_flight_fence[ VK_MAX_FRAMES_IN_FLIGHT ];
    */

} vk_context_t;

/*==============================================================================================
    Global singleton  (shared across all contexts)
==============================================================================================*/

typedef struct vk_state_s
{
    /* global lifecycle */
    bool            initialized;     /* set true after vk_init() completes */

    /* Vulkan globals */
    lib_handle_t    dll;
    VkInstance      instance;
    VkDevice        device;

    /* TODO (Vulkan implementation) -- uncomment as each is wired up:

    VkAllocationCallbacks       alloc_cb;
    VkDebugUtilsMessengerEXT    debug_messenger;

    VkPhysicalDevice            physical_device;
    VkPhysicalDeviceProperties  physical_device_props;
    u32                         graphics_queue_family;
    u32                         present_queue_family;
    VkQueue                     graphics_queue;
    VkQueue                     present_queue;
    */

    /* context pool */
    vk_context_t    contexts[ RHI_CTX_MAX ];
    u32             ctx_alloc;       /* bitmask: bit i set = slot i is live */

} vk_state_t;

static vk_state_t g_vk;

/*============================================================================================*/
// clang-format on
