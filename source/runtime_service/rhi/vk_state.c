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
    rhi_cmd_t  (opaque pointer to one of vk_context_t::cmd_lists[])

    One struct lives inside each context's per-frame slot, no heap allocation needed.
    rhi_cmd_t is a direct pointer to the live slot; NULL = invalid.
==============================================================================================*/

struct rhi_cmd_s
{
    VkCommandBuffer  vk_cmd;        // Vulkan handle; callers record into this directly.
    i32              ctx_id;        // Index of the parent context
    u32              frame;         // slot index [0..VK_MAX_FRAMES_IN_FLIGHT]
};

/*==============================================================================================
    Resource limits
==============================================================================================*/

#define VK_MAX_FRAMES_IN_FLIGHT     2
#define VK_MAX_SWAPCHAIN_IMAGES     3

#define VK_MAX_BUFFERS              1024
#define VK_MAX_TEXTURES             2048
#define VK_MAX_SAMPLERS             128
#define VK_MAX_SHADERS              512
#define VK_MAX_PIPELINES            256

/* Bindless descriptor array sizes (must match shader set layout in vk_descriptor.c) */

#define VK_MAX_BINDLESS_TEXTURES    2048
#define VK_MAX_BINDLESS_SAMPLERS    128

/* Per-frame staging capacity (linear bump; reset each frame_begin) */

#define VK_STAGING_SIZE             ORB_MB( 64 )

/*==============================================================================================
    Resource slot types
==============================================================================================*/

typedef struct vk_buffer_slot_s
{
    VkBuffer            buffer;
    VkDeviceMemory      memory;
    void*               mapped;         // non-NULL only for CPU_TO_GPU / CPU_ONLY allocations
    u32                 size;

} vk_buffer_slot_t;

typedef struct vk_texture_slot_s
{
    VkImage             image;
    VkImageView         view;           // default full-range view
    VkDeviceMemory      memory;
    VkFormat            vk_format;
    u32                 width;
    u32                 height;

} vk_texture_slot_t;

typedef struct vk_sampler_slot_s
{
    VkSampler           sampler;

} vk_sampler_slot_t;

typedef struct vk_shader_slot_s
{
    VkShaderModule      module;  
    rhi_shader_stage_t  stage;
    char                entry[ 32 ];    // SPIR-V entry point name; stored for pipeline create

} vk_shader_slot_t;

typedef struct vk_pipeline_slot_s
{
    VkPipeline          pipeline;       // 
    bool                is_compute;     // 

} vk_pipeline_slot_t;

/*==============================================================================================
    Staging ring  (one per frame-in-flight; active slot tracked by vk_upload.c)
==============================================================================================*/

typedef struct vk_staging_s
{
    VkBuffer       buffer;              // staging: host-visible, coherent, transfer-source only
    VkDeviceMemory memory;              // bound to the staging buffer; persistently mapped for CPU writes.
    void*          mapped;              // persistently mapped host pointer
    u32            head;                // linear bump allocator offset; reset when the slot is flushed
    u64            last_submit_value;   // upload_timeline value signaled when this slot was last submitted

} vk_staging_t;

/*==============================================================================================
    Per-context state  (one per platform window)
==============================================================================================*/

typedef struct vk_context_s
{
    /* Identity */

    i32                 id;                     // our vk.contexts[ id ];
    i32                 win_id;                 // our app.windows[ id ];
    void*               native_window;          // HWND on Windows; cast at use sites

    /* Dimensions */

    i32                 width;                  // current swapchain dimensions, updated by resize.
    i32                 height;                 // used for swapchain creation and viewport setup
    bool                resize_pending;         // swapchain rebuild deferred to next frame_begin */

    /* Frame tracking */

    u32                 current_frame;          // [0..VK_MAX_FRAMES_IN_FLIGHT); indexes per-frame arrays
    u32                 image_index;            // swapchain image acquired by vkAcquireNextImageKHR

    /* Surface and swapchain */

    VkSurfaceKHR        surface;                // created from native_window; used in swapchain and present.
    VkSurfaceFormatKHR  surface_format;         // selected surface format (color space + pixel format)
    VkPresentModeKHR    present_mode;           // selected present mode (vsync / mailbox / immediate)
    VkSwapchainKHR      swapchain;              // created with surface; used in present and image acquisition.
    
    VkExtent2D          swapchain_extent;
    VkImage             swapchain_images        [ VK_MAX_SWAPCHAIN_IMAGES ];
    VkImageView         swapchain_image_views   [ VK_MAX_SWAPCHAIN_IMAGES ];
    u32                 swapchain_image_count;    

    /* Depth attachment: one image per frame-in-flight so consecutive frames do not race
       on the same image. depth_format is shared (same for all slots). */

    VkImage             depth_image             [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkDeviceMemory      depth_memory            [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkImageView         depth_view              [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkFormat            depth_format;

    /* Per-frame synchronization */

    VkSemaphore         image_available_sem     [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkFence             in_flight_fence         [ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Per-swapchain-image: reusing this semaphore is safe only when the image is
       acquired again, which guarantees the previous present consumed it. */

    VkSemaphore         render_finished_sem     [ VK_MAX_SWAPCHAIN_IMAGES ];

    /* Per-frame command state */

    VkCommandPool       command_pool;
    VkCommandBuffer     command_buffers         [ VK_MAX_FRAMES_IN_FLIGHT ];
    struct rhi_cmd_s    cmd_lists               [ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Per-slot layout tracker: UNDEFINED on create; promoted to DEPTH_ATTACHMENT_OPTIMAL after
       each slot's first barrier. Safe because the fence wait guarantees the previous use of
       this slot is complete before we access it again. */

    VkImageLayout       depth_layout            [ VK_MAX_FRAMES_IN_FLIGHT ];

} vk_context_t;

/*==============================================================================================
    Global singleton  (shared across all contexts)
==============================================================================================*/

typedef struct vk_state_s
{    
    VkAllocationCallbacks*              alloc_cb;           // if use_vk_alloc_cb = true     
    VkDebugUtilsMessengerEXT            debug_messenger;    // if use_vk_ext_debug_utils = true.

    /* physical device */

    VkPhysicalDevice                    physical_device;
    VkPhysicalDeviceProperties          physical_device_props;
    VkPhysicalDeviceMemoryProperties    memory_props;

    /* basic flags */
                                               
    u32                     version;                    // full packed VkApiVersion (use VK_VERSION_* macros)
    bool                    initialized;                // global init complete (instance + device)

    bool                    use_vsync;                  // use vsync present mode (else mailbox or immediate)
    bool                    use_vrr_if_available;       // use vrr if available. 
    bool                    has_vrr;                    // system supports variable refresh rate (GSync / FreeSync)
    bool                    use_pipeline_cache;         // load/save pipeline cache to disk (vk_pipeline_cache.c)
    bool                    use_vk_alloc_cb;            // use Vulkan allocation callbacks.
    bool                    use_vk_ext_debug_utils;     // use debug messenger (in DEBUG only)
    bool                    use_vk_layer_validation;    // use vulkan debug layer.
    bool                    use_vk_layer_monitor;       // use vulkan debug layer.

    bool                    ext_win32_surface;          // extension required for win32 window surface support
    bool                    ext_khr_surface;            // extension required for khr window surface support
    
    lib_handle_t            dll;                        // Vulkan loader handle

    /* Core Vulkan objects */

    VkInstance              instance;                   // Our created instance
    VkDevice                device;                     // Our created device

    /* Device capabilities -- cached for faster query */

    VkSampleCountFlagBits   max_msaa_samples;           // max combined color + depth sample count.
    u32                     min_ubo_align;              // minUniformBufferOffsetAlignment, bytes.
    bool                    has_push_descriptor;        // VK_KHR_push_descriptor was enabled.
    bool                    has_fifo_latest_ready;      // VK_KHR_present_mode_fifo_latest_ready was enabled.

    /* Queue families; may be the same index on some hardware */

    u32                     graphics_queue_family;
    u32                     present_queue_family;
    u32                     transfer_queue_family;      // dedicated if available, else graphics
    VkQueue                 graphics_queue;
    VkQueue                 present_queue;
    VkQueue                 transfer_queue;

    /* Pipeline cache (loaded from disk at init; serialized at shutdown for warm restarts) */

    VkPipelineCache         pipeline_cache;

    /* Global bindless descriptor layout (set 0; shared by all pipelines) */

    VkDescriptorSetLayout   bindless_layout;    // layout set 0: of bindless arrays for tex + samp.
    VkDescriptorPool        bindless_pool;      // pool set 0: that supports layout.
    VkDescriptorSet         bindless_set;       // allocated from pool and layout; bound to set 0.
    VkPipelineLayout        pipeline_layout;    // push constants + bindless set 0.

    /* Staging upload ring (active slot owned by vk_upload.c via g_upload_active_slot) */

    vk_staging_t            staging[ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Upload/render sync: timeline semaphore signaled after each DMA batch; render submit waits on it */

    VkSemaphore             upload_timeline;    // monotonic timeline semaphore signaled with each vk_upload_flush; waited on by frame_begin.
    u64                     upload_counter;     // incremented and signaled with each vk_upload_flush; waited on at frame_begin.

    u32                     global_frame;       // monotonic counter; incremented per frame_begin (diagnostics)

    u32                     global_epoch;       // advances when every active context has fence-waited
    u32                     epoch_ack_mask;     // bitmask; context i sets bit i after fence wait; reset on epoch advance

    u32                     upload_flush_epoch; // epoch value at the last vk_upload_flush; gates flush to once per display epoch 
    u32                     acquire_ack_mask;   // bitmask; context i sets bit i after apply_acquires; cleared when all contexts applied

    /* Resource slot pools */

    vk_buffer_slot_t        buffers     [ VK_MAX_BUFFERS ];
    vk_texture_slot_t       textures    [ VK_MAX_TEXTURES ];
    vk_sampler_slot_t       samplers    [ VK_MAX_SAMPLERS ];
    vk_shader_slot_t        shaders     [ VK_MAX_SHADERS ];
    vk_pipeline_slot_t      pipelines   [ VK_MAX_PIPELINES ];

    /* Context pool */

    vk_context_t            contexts    [ RHI_CTX_MAX ];
    u32                     ctx_alloc;                      /* bitmask: bit i set = slot i is live */

} vk_state_t;

/* default values for the global singleton */

static vk_state_t vk =
{
    .use_vsync                  = false,
    .use_vrr_if_available       = true,
    .use_pipeline_cache         = true,
    .use_vk_alloc_cb            = true,
    .use_vk_ext_debug_utils     = true,
    .use_vk_layer_validation    = true,
    .use_vk_layer_monitor       = true,
    .global_epoch               = 1,    /* starts at 1 so upload_flush_epoch=0 triggers the very first flush */
};

/*==============================================================================================
    vk_ctx_get is defined in vk_init.c (included last); forward-declared here so all
    vk_*.c files that follow in the unity build can call it.
==============================================================================================*/

static vk_context_t* vk_ctx_get( i32 id );

/*============================================================================================*/
// clang-format on
