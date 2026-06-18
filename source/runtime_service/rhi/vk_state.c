/*==============================================================================================

    vulkan/vk_state.c -- The engine's memory of all live Vulkan objects.

    Think of this as the engine's address book.  Every texture, buffer, pipeline, window
    swapchain, and sync primitive the engine creates has a slot here.  Nothing in this file
    does GPU work directly -- it defines the shapes of the buckets and holds the one global
    "vk" singleton that every other RHI file reads and writes.

    Included FIRST by rhi.c so every vk_*.c file sees the complete type definitions.

    Three-phase initialization:

        rhi_mod_init          : load vulkan-1.dll; bootstrap Vulkan function pointers.
        rhi()->init()         : create VkInstance + VkDevice (no window required yet).
        rhi()->context_create : per-window surface, swapchain, depth buffer, sync objects,
                                and command buffers.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    rhi_cmd_t  --  a recording handle for one frame's GPU work.

    Think of it like a notepad.  frame_begin hands the caller a fresh page; draw calls and
    barriers are written onto it during the frame; frame_end tears it off and delivers it to
    the GPU.

    Internally it is a direct pointer into one of vk_context_t::cmd_lists[].  Each context
    pre-allocates one slot per frame-in-flight (stack storage; no heap allocation).
    NULL means frame_begin failed; no work should be recorded.
==============================================================================================*/

struct rhi_cmd_s
{
    VkCommandBuffer  vk_cmd;        // Vulkan handle; callers record draw calls into this directly.
    i32              ctx_id;        // Index of the parent context in vk.contexts[].
    u32              frame;         // Frame slot index [0..VK_MAX_FRAMES_IN_FLIGHT).
};

/*==============================================================================================
    Resource limits  --  fixed upper bounds; slots are allocated from flat arrays.
==============================================================================================*/

#define VK_MAX_FRAMES_IN_FLIGHT     2
ORB_STATIC_ASSERT( VK_MAX_FRAMES_IN_FLIGHT == RHI_MAX_FRAMES_IN_FLIGHT,
                   "VK frames-in-flight must match the public RHI_MAX_FRAMES_IN_FLIGHT" );

#define VK_MAX_SWAPCHAIN_IMAGES     3

#define VK_MAX_BUFFERS              1024
#define VK_MAX_TEXTURES             2048
#define VK_MAX_SAMPLERS             128
#define VK_MAX_SHADERS              512
#define VK_MAX_PIPELINES            256

/* Bindless descriptor array sizes (must match shader set layout in vk_descriptor.c) */

#define VK_MAX_BINDLESS_TEXTURES    2048
#define VK_MAX_BINDLESS_SAMPLERS    128

/* Per-frame staging capacity (linear bump; reset at each frame_begin) */

#define VK_STAGING_SIZE             ORB_MB( 64 )

/*==============================================================================================
    Resource slot types  --  one entry per live GPU object; indexed by rhi handle .id field.
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
    VkPipeline          pipeline;
    bool                is_compute;

} vk_pipeline_slot_t;

/*==============================================================================================
    Staging ring  (one slot per frame-in-flight; active slot owned by vk_upload.c)

    Each slot is a 64 MB host-visible buffer the CPU writes into directly via the mapped
    pointer.  The GPU reads from it via vkCmdCopy.  Slots cycle every VK_MAX_FRAMES_IN_FLIGHT
    flushes so a slot is not overwritten until the GPU has finished reading from it.
==============================================================================================*/

typedef struct vk_staging_s
{
    VkBuffer       buffer;              // host-visible, coherent, transfer-source buffer
    VkDeviceMemory memory;              // backing memory; persistently mapped for CPU writes
    void*          mapped;              // CPU-side pointer into the mapped memory
    u32            head;                // linear bump offset; reset to 0 when the slot is flushed
    u64            last_submit_value;   // upload_timeline value signaled when this slot was last
                                        // submitted; checked by vk_staging_alloc before overwriting
} vk_staging_t;

/*==============================================================================================
    Per-context state  --  one instance per platform window.

    A "context" owns everything needed to render into one window: the surface, swapchain,
    depth images, command buffers, and per-frame sync objects.
==============================================================================================*/

typedef struct vk_context_s
{
    /* Identity */

    i32                 id;                     // index into vk.contexts[]
    i32                 win_id;                 // index into app.windows[]
    void*               native_window;          // HWND on Windows; cast at use sites

    /* Dimensions */

    i32                 width;                  // current swapchain width; updated on resize
    i32                 height;                 // current swapchain height; updated on resize
    bool                resize_pending;         // swapchain rebuild deferred to next frame_begin

    /* Frame tracking */

    u32                 current_frame;          // [0..VK_MAX_FRAMES_IN_FLIGHT); ring index into per-frame arrays
    u32                 image_index;            // swapchain image index returned by vkAcquireNextImageKHR

    /* Surface and swapchain */

    VkSurfaceKHR        surface;                // created from native_window; owned for the life of this context
    VkSurfaceFormatKHR  surface_format;         // selected pixel format and color space
    VkPresentModeKHR    present_mode;           // vsync / mailbox / immediate
    VkSwapchainKHR      swapchain;              // pool of presentable images; recreated on resize

    VkExtent2D          swapchain_extent;
    VkImage             swapchain_images        [ VK_MAX_SWAPCHAIN_IMAGES ];
    VkImageView         swapchain_image_views   [ VK_MAX_SWAPCHAIN_IMAGES ];
    u32                 swapchain_image_count;

    /* Depth attachment: one image per frame-in-flight so consecutive frames do not race
       on the same depth image.  depth_format is shared across all slots. */

    VkImage             depth_image             [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkDeviceMemory      depth_memory            [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkImageView         depth_view              [ VK_MAX_FRAMES_IN_FLIGHT ];
    VkFormat            depth_format;

    /* --- Per-frame synchronization ---

       Three primitives work together like a relay race to keep the CPU, GPU, and display
       engine from tripping over each other.

       (1) in_flight_fence      The CPU blocks here at frame_begin until the GPU finishes
                                whatever it was doing with this frame slot last time.
                                Prevents overwriting a command buffer the GPU still reads.

       (2) image_available_sem  The GPU stalls COLOR_ATTACHMENT_OUTPUT until the display
                                engine releases the swapchain image.  Usually already
                                satisfied before the GPU reaches that stage; only stalls in
                                the rare case where the display is still reading the image.

       (3) render_finished_sem  The GPU signals this when all draw commands complete.
                                vkQueuePresentKHR waits on it so the display engine will
                                not scan out the image before the GPU finishes writing it.
    */

    /* Fence the GPU signals when it finishes all work in this frame slot.
       The CPU waits on it at frame_begin before reusing the slot's command buffer,
       staging memory, and depth image. */

    VkFence             in_flight_fence         [ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Semaphore the WSI signals when a swapchain image is free for rendering. */

    VkSemaphore         image_available_sem     [ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Semaphore the GPU signals when rendering is complete; vkQueuePresentKHR waits on
       it before handing the image to the display engine for scan-out. */

    VkSemaphore         render_finished_sem     [ VK_MAX_SWAPCHAIN_IMAGES ];

    /* --- Per-frame command state --- */

    VkCommandPool       command_pool;
    VkCommandBuffer     command_buffers         [ VK_MAX_FRAMES_IN_FLIGHT ];
    struct rhi_cmd_s    cmd_lists               [ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Tracks the current Vulkan layout of each slot's depth image.  Starts as UNDEFINED;
       promoted to DEPTH_ATTACHMENT_OPTIMAL after the first barrier in frame_begin. 
       Safe to read here because the fence wait guarantees the prior use of this slot is done. */

    VkImageLayout       depth_layout            [ VK_MAX_FRAMES_IN_FLIGHT ];

} vk_context_t;

/*==============================================================================================
    Global singleton  --  shared state visible to all contexts and all vk_*.c files.
==============================================================================================*/

typedef struct vk_state_s
{
    VkAllocationCallbacks*              alloc_cb;           // custom allocator; NULL in non-debug builds
    VkDebugUtilsMessengerEXT            debug_messenger;    // validation layer message callback

    /* Physical device */

    VkPhysicalDevice                    physical_device;
    VkPhysicalDeviceProperties          physical_device_props;
    VkPhysicalDeviceMemoryProperties    memory_props;

    /* Flags and feature toggles */

    u32                     version;                    // packed VkApiVersion (use VK_VERSION_* macros)
    bool                    initialized;                // true after instance + device are created

    bool                    use_vsync;                  // FIFO present mode (hard vsync)
    bool                    use_vrr_if_available;       // prefer variable refresh rate if supported
    bool                    has_vrr;                    // true if GSync / FreeSync was detected
    bool                    use_pipeline_cache;         // serialize pipeline cache to disk for warm restarts
    bool                    use_vk_alloc_cb;            // enable Vulkan allocation callbacks
    bool                    use_vk_ext_debug_utils;     // enable debug messenger (debug builds only)
    bool                    use_vk_layer_validation;    // enable Vulkan validation layer
    bool                    use_vk_layer_monitor;       // enable Vulkan monitor layer

    bool                    ext_win32_surface;          // VK_KHR_win32_surface instance extension
    bool                    ext_khr_surface;            // VK_KHR_surface instance extension

    lib_handle_t            dll;                        // vulkan-1.dll handle

    /* Core Vulkan objects */

    VkInstance              instance;
    VkDevice                device;

    /* Device capability cache */

    VkSampleCountFlagBits   max_msaa_samples;           // highest combined color + depth sample count
    u32                     min_ubo_align;              // minUniformBufferOffsetAlignment, in bytes
    bool                    has_push_descriptor;        // VK_KHR_push_descriptor was enabled
    bool                    has_fifo_latest_ready;      // VK_KHR_present_mode_fifo_latest_ready was enabled

    /* Queue families -- may be the same index on integrated hardware */

    u32                     graphics_queue_family;
    u32                     present_queue_family;
    u32                     transfer_queue_family;      // dedicated DMA engine if available; else graphics
    VkQueue                 graphics_queue;
    VkQueue                 present_queue;
    VkQueue                 transfer_queue;

    /* Pipeline cache (loaded from disk at init; saved at shutdown for warm restarts) */

    VkPipelineCache         pipeline_cache;

    /* Global bindless descriptor layout (set 0; shared by all pipelines) */

    VkDescriptorSetLayout   bindless_layout;    // set 0 layout: arrays of textures and samplers
    VkDescriptorPool        bindless_pool;      // pool backing the bindless set
    VkDescriptorSet         bindless_set;       // the one descriptor set bound to all pipelines
    VkPipelineLayout        pipeline_layout;    // set as bindless set 0 + push constants

    /* Staging upload ring (VK_MAX_FRAMES_IN_FLIGHT slots; active slot owned by vk_upload.c) */

    vk_staging_t            staging[ VK_MAX_FRAMES_IN_FLIGHT ];

    /* Upload sync: one timeline semaphore connects the transfer queue to the graphics queue.

       A timeline semaphore is a 64-bit counter that both the CPU and GPU can observe.
       Every time vk_upload_flush() submits a DMA batch, upload_counter increments and the
       transfer queue signals that value when the copy finishes.  frame_end then tells the
       graphics queue "stall vertex input and shaders until upload_timeline reaches
       upload_counter" -- so shaders always see fully uploaded data.
       Frames with no uploads skip the wait entirely (guarded by the upload_counter > 0 check
       in frame_end).                                                                         */

    VkSemaphore             upload_timeline;    // timeline semaphore; transfer queue signals this after each flush
    u64                     upload_counter;     // CPU mirror of the last signaled timeline value
    bool                    upload_batch_open;  // true while a transfer batch is recording (will signal upload_counter+1)

    /* Display epoch: gates vk_upload_flush() to fire at most once per display frame,
       regardless of how many contexts call frame_begin in the same frame.

       Each context sets its bit in epoch_ack_mask after its fence wait.  When every active
       context has checked in, global_epoch advances and the mask resets.  upload_flush_epoch
       records when the flush last ran; a context only triggers the flush when
       upload_flush_epoch < global_epoch, preventing redundant flushes in multi-context setups. */

    u32                     global_epoch;       // advances when all active contexts complete their fence wait
    u32                     epoch_ack_mask;     // bitmask: context i sets bit i; cleared when all check in
    u32                     upload_flush_epoch; // global_epoch value at the last vk_upload_flush call

    /* QFOT acquires are injected exactly once -- into the first graphics command buffer recorded
       after vk_upload_flush submits the matching releases (see vk_upload.c) -- so no per-context
       acknowledgement bitmask is needed here. */

    /* Resource slot pools  --  indexed by the .id field of the corresponding rhi handle */

    vk_buffer_slot_t        buffers     [ VK_MAX_BUFFERS ];
    vk_texture_slot_t       textures    [ VK_MAX_TEXTURES ];
    vk_sampler_slot_t       samplers    [ VK_MAX_SAMPLERS ];
    vk_shader_slot_t        shaders     [ VK_MAX_SHADERS ];
    vk_pipeline_slot_t      pipelines   [ VK_MAX_PIPELINES ];

    /* Context pool */

    vk_context_t            contexts    [ RHI_CTX_MAX ];
    u32                     ctx_alloc;          // bitmask: bit i set = slot i is live

} vk_state_t;

/* Default values for the global singleton.
   global_epoch starts at 1 so upload_flush_epoch=0 fires the flush on the very first frame. */

static vk_state_t vk =
{
    .use_vsync                  = false,
    .use_vrr_if_available       = true,
    .use_pipeline_cache         = true,
    .use_vk_alloc_cb            = false,
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
