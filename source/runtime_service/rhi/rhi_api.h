#ifndef RHI_API_H
#define RHI_API_H
/*==============================================================================================

    runtime_service/rhi/rhi_api.h -- RHI module API struct and gateway macro.

    Function groups (all called through the rhi() vtable):
        Lifecycle  : init / shutdown
        Context    : create / destroy / resize  (one per window)
        Frame      : begin / end
        Buffer     : create / destroy / write
        Texture    : create / destroy
        Sampler    : create / destroy
        Shader     : create / destroy / load_file / load_memory
        Pipeline   : create / compute_create / destroy
        Upload     : staged copy to GPU-only buffer or texture
        Pass       : begin_rendering / end_rendering  (explicit dynamic pass open/close)
        Commands   : viewport, scissor, pipeline, vertex/index, push constants, draw
        Bindless   : register/unregister texture+sampler indices; bind global set
        Debug      : begin/end GPU label  (debug builds only; release no-ops)

==============================================================================================*/

#include "runtime_service/rhi/rhi.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct rhi_api_s
{
    /* ---- Global lifecycle (once per process) ---- */

    /* Creates VkInstance, selects a physical device, opens VkDevice, allocates descriptor
       pools, and sets up the shared bindless pipeline layout.
       Returns false if Vulkan 1.3 is unavailable or a required extension is missing.
       No window or surface is needed; call this before context_create(). */
    bool (*init)( void );

    /* Destroys VkDevice, VkInstance, and all shared Vulkan state created by init().
       Every context must be destroyed with context_destroy() before calling this.
       Calling any RHI function after shutdown() is undefined behavior. */
    void (*shutdown)( void );

    /* ---- Per-context lifecycle (one per window) ---- */

    /* Creates a render context for a platform window: VkSurfaceKHR, VkSwapchainKHR,
       per-frame command pools, semaphores, fences, and a shared depth buffer.
       native_window is a platform handle (HWND on Windows, xcb_window_t on Linux).
       w and h are the initial drawable size in pixels.
       Returns a non-negative context ID on success, RHI_CTX_INVALID on failure. */
    i32 (*context_create)( i32 win_id, void* native_window, i32 w, i32 h );

    /* Destroys all per-context Vulkan state and returns the slot to the pool.
       Must be called before shutdown().  Any rhi_cmd_t from this context is invalid
       after this call. */
    void (*context_destroy)( i32 ctx_id );

    /* Recreates the swapchain at the new pixel dimensions.  Call when the OS reports a
       window resize; do not call every frame.  In-flight frame commands are flushed before
       the swapchain is replaced.  Returns false if recreation fails (surface lost, OOM). */
    bool (*context_resize)( i32 ctx_id, i32 w, i32 h );

    /* ---- Frame ---- */

    /* Acquires the next swapchain image and opens the frame command buffer.
       Returns RHI_CMD_INVALID when the swapchain is not ready (minimized window, suboptimal
       surface); in that case skip rendering AND skip frame_end() for this frame (the command
       buffer was never begun).  frame_begin() must still be called every frame on every live
       context regardless of result -- it performs the per-context epoch check-in that advances
       upload flushing and deferred reclaim; skipping it on one context stalls all of them.
       The returned rhi_cmd_t is valid only until the matching frame_end() call. */
    rhi_cmd_t (*frame_begin)( i32 ctx_id );

    /* Submits the frame command buffer and queues the swapchain image for presentation.
       Call exactly once after each frame_begin() that returned a VALID command list; do NOT
       call it when frame_begin() returned RHI_CMD_INVALID. */
    void (*frame_end)( i32 ctx_id );

    /* Returns the frame-in-flight slot index [0, RHI_MAX_FRAMES_IN_FLIGHT) that the
       given command list records into.  Use it to select the per-frame region of an
       N-buffered dynamic resource before writing and binding it.  Returns 0 for an
       invalid command list. */
    u32 (*cmd_frame_index)( rhi_cmd_t cmd );

    /* ---- Buffer ---- */

    /* Allocates a GPU buffer according to desc.  Memory type and usage flags must be
       consistent: GPU_ONLY is device-local and cannot be CPU-mapped (use upload_buffer()
       to fill it); CPU_TO_GPU is host-visible and suitable for per-frame streaming data
       such as uniforms, vertices, and indirect draw args; CPU_ONLY is a staging source
       for upload_buffer() and should not be bound to shaders.
       Returns an invalid handle (id == 0) on allocation failure. */
    rhi_buffer_t (*buffer_create)( const rhi_buffer_desc_t* desc );

    /* Frees the buffer and returns the slot to the pool.
       Passing an invalid handle is a no-op. */
    void (*buffer_destroy)( rhi_buffer_t buf );

    /* Copies size bytes from data into buf starting at byte offset.
       Only valid for CPU_TO_GPU and CPU_ONLY memory; behavior is undefined on GPU_ONLY.
       The write is visible to commands recorded after this call with no explicit flush. */
    void (*buffer_write)( rhi_buffer_t buf, const void* data, u32 size, u32 offset );

    /* Returns the 64-bit GPU virtual address of a buffer created with
       RHI_BUFFER_USAGE_DEVICE_ADDRESS.  Returns 0 if the handle is invalid or the flag
       was not set.  Pass this address in push constants so shaders can access the buffer
       without a descriptor binding (BDA / raw pointer style). */
    u64 (*buffer_get_device_address)( rhi_buffer_t buf );

    /* ---- Texture ---- */

    /* Allocates a Vulkan image and a matching VkImageView.
       mip_levels = 0 auto-computes the full mip chain from width/height.
       array_layers = 1 for a plain 2D texture; >1 for a 2D array.
       New textures are in VK_IMAGE_LAYOUT_UNDEFINED.  Fill them with upload_texture()
       or transition them explicitly before sampling.
       Returns an invalid handle on failure. */
    rhi_texture_t (*texture_create)( const rhi_texture_desc_t* desc );

    /* Destroys the image, image view, and backing allocation.
       Unregister from the bindless set with unregister_texture() before destroying
       a texture that was previously registered. */
    void (*texture_destroy)( rhi_texture_t tex );

    /* ---- Sampler ---- */

    /* Creates a VkSampler from the descriptor.  Samplers are lightweight; create one
       per distinct filter+wrap combination and share it across textures.
       Returns an invalid handle on failure. */
    rhi_sampler_t (*sampler_create)( const rhi_sampler_desc_t* desc );

    /* Destroys the sampler.  Unregister from the bindless set with unregister_sampler()
       before destroying a sampler that was previously registered. */
    void (*sampler_destroy)( rhi_sampler_t samp );

    /* ---- Shader ---- */

    /* Creates a VkShaderModule from the pre-compiled SPIR-V bytecode in desc->spirv.
       Returns an invalid handle if SPIR-V validation fails.
       Shaders are only needed during pipeline compilation; once a pipeline is created
       the shader module can be destroyed immediately. */
    rhi_shader_t (*shader_create)( const rhi_shader_desc_t* desc );

    /* Destroys the VkShaderModule.  Safe to call as soon as the pipeline is created. */
    void (*shader_destroy)( rhi_shader_t shader );

    /* Reads a compiled .spv file from disk and delegates to shader_create().
       stage must match the shader's actual stage (VERTEX, FRAGMENT, or COMPUTE).
       entry is the SPIR-V entry point name; "main" is conventional.
       Returns an invalid handle if the file cannot be read or the SPIR-V is invalid. */
    rhi_shader_t (*shader_load_file)( const char* path, rhi_shader_stage_t stage,
                                      const char* entry, const char* debug_name );

    /* Creates a shader from an in-memory SPIR-V byte array (e.g. a static C array).
       Use for shaders embedded at compile time to avoid a file-system dependency.
       size is the byte count of the spirv buffer (not element count).
       Returns an invalid handle if the SPIR-V is invalid. */
    rhi_shader_t (*shader_load_memory)( const void* spirv, u32 size, rhi_shader_stage_t stage,
                                        const char* entry, const char* debug_name );

    /* ---- Pipeline ---- */

    /* Compiles and links a graphics VkPipeline using dynamic rendering (no VkRenderPass).
       color target formats, depth_format, and push_const_size declared in desc must match
       exactly what is used at draw time -- mismatches produce undefined rendering.
       Pipeline compilation can be slow; build pipelines at load time, not per frame.
       Returns an invalid handle if shader compilation or pipeline linking fails. */
    rhi_pipeline_t (*pipeline_create)( const rhi_pipeline_desc_t* desc );

    /* Compiles a compute VkPipeline.  The resulting handle is used with cmd_bind_pipeline()
       and cmd_dispatch() exactly like a graphics pipeline; the backend selects the correct
       VkPipelineBindPoint automatically.
       Returns an invalid handle on failure. */
    rhi_pipeline_t (*compute_pipeline_create)( const rhi_compute_pipeline_desc_t* desc );

    /* Destroys the pipeline state object and returns the slot to the pool.
       Passing an invalid handle is a no-op. */
    void (*pipeline_destroy)( rhi_pipeline_t pipeline );

    /* ---- Staged upload (GPU_ONLY targets) ---- */

    /* Enqueues an asynchronous copy of size bytes from data into GPU_ONLY buffer dst via
       an internal staging buffer.  The upload is flushed at the frame_begin() that reuses
       this staging slot (VK_MAX_FRAMES_IN_FLIGHT frames later).  The buffer is safe to
       read in commands recorded during that same frame; reading it earlier yields stale data.
       Returns false if the staging pool is exhausted. */
    bool (*upload_buffer)( rhi_buffer_t dst, const void* data, u32 size );

    /* Enqueues an asynchronous copy of data_size bytes into the given mip level and array
       layer of GPU_ONLY texture dst.  Applies the necessary layout transition to
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL before the texture becomes readable.
       The texture is safe to sample starting in the frame the upload is flushed.
       Returns false if the staging pool is exhausted. */
    bool (*upload_texture)( rhi_texture_t dst, const void* data, u32 data_size, u16 mip, u16 layer );

    /* ---- Render pass ---- */

    /* Opens a dynamic rendering pass (vkCmdBeginRendering).
       color_atts points to color_count color attachment descriptors; depth_att may be NULL
       to skip depth testing and writing.  Use the sentinel IDs RHI_SWAPCHAIN_COLOR and
       RHI_SWAPCHAIN_DEPTH as the texture field to target the context's swapchain image or
       built-in depth buffer respectively.
       The pass must be closed with cmd_end_rendering() before frame_end() or before
       opening another pass. */
    void (*cmd_begin_rendering)( rhi_cmd_t cmd, const rhi_color_attachment_t* color_atts,
                                 u32 color_count, const rhi_depth_attachment_t* depth_att );

    /* Closes the dynamic rendering pass (vkCmdEndRendering).
       Must be matched with every cmd_begin_rendering() call in the same frame. */
    void (*cmd_end_rendering)( rhi_cmd_t cmd );

    /* Transitions one or more user-created textures between pipeline layouts, inserting
       the required execution and memory dependencies via vkCmdPipelineBarrier2.
       Call between cmd_end_rendering and the next cmd_begin_rendering; never inside an
       open render pass.  Batching multiple textures in a single call is more efficient
       than one call per texture; all barriers are submitted in one pipeline stall.
       Same-layout transitions and invalid handles are silently skipped.
       Do not pass swapchain or context-owned depth targets here; those are managed
       internally and have no rhi_texture_t handle. */
    void (*cmd_image_barrier)( rhi_cmd_t cmd, const rhi_image_barrier_t* barriers, u32 count );

    /* ---- Commands ---- */

    /* Sets the viewport transform applied after vertex processing.
       min_depth / max_depth are typically 0.0 / 1.0.
       To match OpenGL-style top-left clip origin, set vp->y = height and vp->height = -height;
       the pipeline must use VK_KHR_maintenance1 negated-height convention consistently. */
    void (*cmd_set_viewport)( rhi_cmd_t cmd, const rhi_viewport_t* vp );

    /* Sets the scissor rectangle; pixels outside the rect are discarded before output merger.
       Coordinates are in pixel space relative to the attachment origin.
       Call after cmd_set_viewport() when the scissor region changes between draws. */
    void (*cmd_set_scissor)( rhi_cmd_t cmd, const rhi_rect_t* rect );

    /* Binds a graphics or compute pipeline.  All subsequent draws or dispatches use this
       pipeline until a different one is bound.  Graphics pipelines must be bound inside an
       open render pass; compute pipelines must be bound outside a render pass. */
    void (*cmd_bind_pipeline)( rhi_cmd_t cmd, rhi_pipeline_t pipeline );

    /* Binds a vertex buffer at byte offset into buf.  Only a single interleaved vertex
       binding is supported.  Call before each draw that reads from a different buffer or
       at a different offset. */
    void (*cmd_bind_vertex_buffer)( rhi_cmd_t cmd, rhi_buffer_t buf, u32 offset );

    /* Binds an index buffer.  type selects 16-bit or 32-bit indices.
       offset is in bytes from the start of buf.  Required before any indexed draw call. */
    void (*cmd_bind_index_buffer)( rhi_cmd_t cmd, rhi_buffer_t buf, u32 offset,
                                   rhi_index_type_t type );

    /* Writes bytes [offset, offset+size) of the push constant block from data.
       size must not exceed RHI_MAX_PUSH_CONST_SIZE (128 bytes guaranteed by the Vulkan spec)
       and offset+size must fit within the range declared in the bound pipeline.
       No synchronization is needed; the values are visible to the very next draw or dispatch. */
    void (*cmd_push_constants)( rhi_cmd_t cmd, const void* data, u32 size, u32 offset );

    /* Records a non-indexed draw call.  Set args->instance_count = 1 for a single instance. */
    void (*cmd_draw)( rhi_cmd_t cmd, const rhi_draw_args_t* args );

    /* Records an indexed draw call.  A vertex and index buffer must be bound before calling.
       args->vertex_offset is added to every fetched index before the vertex buffer lookup. */
    void (*cmd_draw_indexed)( rhi_cmd_t cmd, const rhi_draw_indexed_args_t* args );

    /* Issues draw_count non-indexed indirect draws, reading one VkDrawIndirectCommand per
       draw from buf at byte offset, strided by stride bytes.
       buf must have RHI_BUFFER_USAGE_INDIRECT set.
       stride is typically sizeof(VkDrawIndirectCommand) = 16 bytes. */
    void (*cmd_draw_indirect)( rhi_cmd_t cmd, rhi_buffer_t buf,
                               u32 offset, u32 draw_count, u32 stride );

    /* Issues draw_count indexed indirect draws, reading one VkDrawIndexedIndirectCommand per
       draw from buf.  A bound index buffer is required in addition to INDIRECT usage on buf.
       stride is typically sizeof(VkDrawIndexedIndirectCommand) = 20 bytes. */
    void (*cmd_draw_indexed_indirect)( rhi_cmd_t cmd, rhi_buffer_t buf,
                                       u32 offset, u32 draw_count, u32 stride );

    /* Dispatches (groups_x * groups_y * groups_z) compute workgroups.
       A compute pipeline must be bound and no render pass may be open when calling this.
       Workgroup dimensions are shader-defined; groups_* is the count of workgroups, not threads. */
    void (*cmd_dispatch)( rhi_cmd_t cmd, u32 groups_x, u32 groups_y, u32 groups_z );

    /* ---- Bindless resource registration ---- */

    /* Registers a texture in the global bindless descriptor array and returns a persistent
       slot index.  Pass this index via push constants; shaders look it up through the bindless
       set to sample the texture without a per-draw descriptor update.
       Returns 0 (invalid) if the pool is exhausted.  The index is stable until the matching
       unregister_texture() call. */
    u32 (*register_texture)( rhi_texture_t tex );

    /* Releases the bindless texture slot.  The slot may be reused by a subsequent
       register_texture() call.  Do not reference the index in shaders after unregistering. */
    void (*unregister_texture)( u32 bindless_index );

    /* Registers a sampler in the global bindless sampler array and returns a persistent slot
       index, or 0 if the pool is exhausted.  Same lifetime rules as register_texture(). */
    u32 (*register_sampler)( rhi_sampler_t samp );

    /* Releases the bindless sampler slot.  Same rules as unregister_texture(). */
    void (*unregister_sampler)( u32 bindless_index );

    /* Binds the global bindless descriptor set (set 0) to the currently bound pipeline.
       Call once per frame after binding a pipeline and before the first draw or dispatch
       that reads bindless resources.  Re-bind after every pipeline switch. */
    void (*cmd_bind_bindless)( rhi_cmd_t cmd );

    /* ---- Debug GPU labels (no-ops in release; always safe to call) ---- */

    /* Opens a GPU debug label visible in RenderDoc, PIX, NSight, and other GPU profilers.
       r, g, b are the label color in linear [0.0, 1.0].  Labels nest; open/close pairs must
       balance within a frame.  Compiled out entirely in release builds. */
    void (*cmd_begin_label)( rhi_cmd_t cmd, const char* name, f32 r, f32 g, f32 b );

    /* Closes the most recently opened GPU debug label.
       Must be matched with every cmd_begin_label() call in the same frame. */
    void (*cmd_end_label)( rhi_cmd_t cmd );

} rhi_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( RHI_STATIC )
MOD_GATEWAY_STATIC( rhi_api_t, rhi )
#else
MOD_GATEWAY_DYNAMIC( rhi_api_t, rhi )
#endif

#if defined( BUILD_STATIC ) || defined( RHI_STATIC )
    #define MOD_USE_RHI    /* static build */
    #define MOD_FETCH_RHI  true
#else
    #define MOD_USE_RHI    MOD_DEFINE_API_PTR( rhi_api_t, rhi )
    #define MOD_FETCH_RHI  MOD_FETCH_API( rhi_api_t, rhi )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RHI_API_H
