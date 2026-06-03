/*==============================================================================================

    vulkan/vk_state.c -- Vulkan state: opaque command list type, per-context state, and
    the global vk singleton.

    Included FIRST by rhi.c so every other vk_*.c file sees the complete type definitions.

    Initialization is three-phase:
        rhi_mod_init           : loads vulkan-1.dll (vk_lib_init)
        rhi()->init()          : VkInstance + VkDevice + global resources (no window)
        rhi()->context_create  : per-window surface, swapchain, depth buffer, sync, commands

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Handle helpers  (internal; never exposed in rhi.h)

    id layout: [ gen:8 | idx:24 ]
    id == 0 is always null (RHI_NULL_HANDLE).
    Generations start at 1 so slot 0 at gen 1 gives id != 0.
==============================================================================================*/

#define VK_HANDLE_IDX( id )          ( (u32)( (id) & 0x00FFFFFFu ) )
#define VK_HANDLE_GEN( id )          ( (u8)(  (id) >> 24           ) )
#define VK_MAKE_HANDLE( gen, idx )   ( ( (u32)(gen) << 24 ) | (u32)(idx) )

/*==============================================================================================
    rhi_command_list_s  (opaque to callers via rhi_command_list_t pointer)

    One struct lives inside each context's per-frame slot, so no heap allocation is needed.
    frame_begin returns a pointer into ctx->cmd_lists[current_frame].
==============================================================================================*/

struct rhi_command_list_s
{
    VkCommandBuffer  vk_cmd;
    i32              ctx_id;
    u32              frame;    /* slot index [0..VK_MAX_FRAMES_IN_FLIGHT) */
};

/*==============================================================================================
    Resource limits
==============================================================================================*/

#define VK_MAX_FRAMES_IN_FLIGHT   2
#define VK_MAX_SWAPCHAIN_IMAGES   8

#define VK_MAX_BUFFERS            1024
#define VK_MAX_TEXTURES           2048
#define VK_MAX_SAMPLERS           128
#define VK_MAX_SHADERS            512
#define VK_MAX_PIPELINES          256

/* Bindless descriptor array sizes (must match shader set layout in vk_descriptor.c) */
#define VK_MAX_BINDLESS_TEXTURES  2048
#define VK_MAX_BINDLESS_SAMPLERS  128

/* Per-frame staging capacity (linear bump; reset each frame_begin) */
#define VK_STAGING_SIZE           ORB_MB( 64 )

/*==============================================================================================
    Resource slot types
==============================================================================================*/

typedef struct vk_buffer_slot_s
{
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void*          mapped;       /* non-NULL only for CPU_TO_GPU / CPU_ONLY allocations */
    u32            size;
    u8             generation;   /* 0 = free slot */

} vk_buffer_slot_t;

typedef struct vk_texture_slot_s
{
    VkImage        image;
    VkImageView    view;         /* default full-range view */
    VkDeviceMemory memory;
    VkFormat       vk_format;
    u32            width;
    u32            height;
    u8             generation;

} vk_texture_slot_t;

typedef struct vk_sampler_slot_s
{
    VkSampler  sampler;
    u8         generation;

} vk_sampler_slot_t;

typedef struct vk_shader_slot_s
{
    VkShaderModule     module;
    rhi_shader_stage_t stage;
    u8                 generation;

} vk_shader_slot_t;

typedef struct vk_pipeline_slot_s
{
    VkPipeline  pipeline;
    u8          generation;

} vk_pipeline_slot_t;

/*==============================================================================================
    Staging ring  (one per frame-in-flight; indexed by global frame_index % VK_MAX_FRAMES)
==============================================================================================*/

typedef struct vk_staging_s
{
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void*          mapped;     /* persistently mapped host pointer */
    u32            head;       /* linear bump allocator offset; reset each frame_begin */

} vk_staging_t;

/*==============================================================================================
    Per-context state  (one per platform window)
==============================================================================================*/

typedef struct vk_context_s
{
    /* Identity */
    i32    id;
    i32    win_id;
    void*  native_window;     /* HWND on Windows; cast at use sites */

    /* Dimensions */
    i32    width;
    i32    height;
    bool   resize_pending;    /* swapchain rebuild deferred to next frame_begin */

    /* Frame tracking */
    u32    current_frame;     /* [0..VK_MAX_FRAMES_IN_FLIGHT); indexes per-frame arrays */
    u32    image_index;       /* swapchain image acquired by vkAcquireNextImageKHR */

    /* Surface and swapchain */
    VkSurfaceKHR        surface;
    VkSurfaceFormatKHR  surface_format;
    VkPresentModeKHR    present_mode;
    VkSwapchainKHR      swapchain;
    u32                 swapchain_image_count;
    VkImage             swapchain_images[ VK_MAX_SWAPCHAIN_IMAGES ];
    VkImageView         swapchain_image_views[ VK_MAX_SWAPCHAIN_IMAGES ];
    VkExtent2D          swapchain_extent;

    /* Depth attachment (matched to swapchain extent) */
    VkImage        depth_image;
    VkDeviceMemory depth_memory;
    VkImageView    depth_view;
    VkFormat       depth_format;    /* selected at swapchain creation time */

    /* Per-frame synchronization */
    VkSemaphore  image_available_sem[ VK_MAX_FRAMES_IN_FLIGHT ];
    VkSemaphore  render_finished_sem[ VK_MAX_FRAMES_IN_FLIGHT ];
    VkFence      in_flight_fence[ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Per-frame command state */
    VkCommandPool            command_pool;
    VkCommandBuffer          command_buffers[ VK_MAX_FRAMES_IN_FLIGHT ];
    struct rhi_command_list_s cmd_lists[ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Clear color (set by cmd_clear_color; consumed by vkCmdBeginRendering loadOp) */
    VkClearColorValue  clear_color;

} vk_context_t;

/*==============================================================================================
    Global singleton  (shared across all contexts)
==============================================================================================*/

typedef struct vk_state_s
{
    i32  version;                           // minor version number

    /* status flags */

    bool    initialized;    
    bool    use_vk_alloc_cb;                // use Vulkan allocation callbacks.
    bool    use_vk_ext_debug_utils;         // use debug messenger.
    bool    use_vk_layer_validation;        // use vulkan debug layer.
    bool    use_vk_layer_monitor;           // use vulkan debug layer.

    bool    ext_win32_surface;              // extention required for win32 window surface support
    bool    ext_khr_surface;                // extention required for win32 window surface support

    /* Vulkan loader handle */
    lib_handle_t  dll;

    /* Core Vulkan objects */
    VkInstance   instance;
    VkDevice     device;

    /* Allocation callbacks (NULL = Vulkan default allocator) */

    VkAllocationCallbacks*              alloc_cb;

    /* Debug messenger (enabled when DEBUG and VK_EXT_debug_utils is available) */

    VkDebugUtilsMessengerEXT            debug_messenger;

    /* Physical device */

    VkPhysicalDevice                    physical_device;
    VkPhysicalDeviceProperties          physical_device_props;
    VkPhysicalDeviceMemoryProperties    memory_props;

    /* Queue families; may be the same index on some hardware */

    u32         graphics_queue_family;
    u32         present_queue_family;
    u32         transfer_queue_family;    /* dedicated transfer queue if available, else graphics */
    VkQueue     graphics_queue;
    VkQueue     present_queue;
    VkQueue     transfer_queue;

    /* Pipeline cache (loaded from disk at init; serialized at shutdown for warm restarts) */

    VkPipelineCache  pipeline_cache;

    /* Global bindless descriptor layout (set 0; shared by all pipelines) */

    VkDescriptorPool       bindless_pool;
    VkDescriptorSetLayout  bindless_layout;
    VkDescriptorSet        bindless_set;
    VkPipelineLayout       pipeline_layout;   /* push constants + bindless set 0 */

    /* Staging upload ring (indexed by global_frame % VK_MAX_FRAMES_IN_FLIGHT) */

    vk_staging_t  staging[ VK_MAX_FRAMES_IN_FLIGHT ];
    u32           global_frame;   /* monotonic counter; drives staging slot selection */

    /* Resource slot pools */

    vk_buffer_slot_t    buffers[ VK_MAX_BUFFERS ];
    vk_texture_slot_t   textures[ VK_MAX_TEXTURES ];
    vk_sampler_slot_t   samplers[ VK_MAX_SAMPLERS ];
    vk_shader_slot_t    shaders[ VK_MAX_SHADERS ];
    vk_pipeline_slot_t  pipelines[ VK_MAX_PIPELINES ];

    /* Context pool */

    vk_context_t  contexts[ RHI_CTX_MAX ];
    u32           ctx_alloc;   /* bitmask: bit i set = slot i is live */

} vk_state_t;

static vk_state_t vk =
{
    .use_vk_alloc_cb = true,
    .use_vk_ext_debug_utils = true,
    .use_vk_layer_validation = true,
    .use_vk_layer_monitor = true,
};

/*============================================================================================*/
// clang-format on
