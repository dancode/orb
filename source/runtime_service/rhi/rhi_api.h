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

    bool ( *init     )( void );   /* create VkInstance + VkDevice; no window needed */
    void ( *shutdown )( void );   /* call after all contexts are destroyed */

    /* ---- Per-context lifecycle (one per window) ---- */

    i32  ( *context_create  )( i32 win_id, void* native_window, i32 w, i32 h );
    void ( *context_destroy )( i32 ctx_id );
    bool ( *context_resize  )( i32 ctx_id, i32 w, i32 h );

    /* ---- Frame ---- */

    rhi_command_list_t ( *frame_begin )( i32 ctx_id );   /* NULL = swapchain not ready */
    void               ( *frame_end   )( i32 ctx_id );

    /* ---- Buffer ---- */

    rhi_buffer_t ( *buffer_create  )( const rhi_buffer_desc_t* desc );
    void         ( *buffer_destroy )( rhi_buffer_t buf );

    /* Write directly to a CPU_TO_GPU or CPU_ONLY buffer.  Undefined on GPU_ONLY memory. */
    void         ( *buffer_write           )( rhi_buffer_t buf, const void* data, u32 size, u32 offset );

    /* Returns the 64-bit GPU virtual address of a DEVICE_ADDRESS buffer; 0 on invalid handle.
       Pass this value in push constants to let shaders access the buffer without a descriptor. */
    u64          ( *buffer_get_device_address )( rhi_buffer_t buf );

    /* ---- Texture ---- */

    rhi_texture_t ( *texture_create  )( const rhi_texture_desc_t* desc );
    void          ( *texture_destroy )( rhi_texture_t tex );

    /* ---- Sampler ---- */

    rhi_sampler_t ( *sampler_create  )( const rhi_sampler_desc_t* desc );
    void          ( *sampler_destroy )( rhi_sampler_t samp );

    /* ---- Shader ---- */

    rhi_shader_t  ( *shader_create      )( const rhi_shader_desc_t* desc );
    void          ( *shader_destroy     )( rhi_shader_t shader );

    /* Convenience loaders -- both delegate to shader_create internally.
       shader_load_file  reads a compiled .spv file from disk.
       shader_load_memory creates from an embedded SPIR-V byte array (e.g. a static C array). */
    rhi_shader_t  ( *shader_load_file   )( const char* path, rhi_shader_stage_t stage,
                                            const char* entry, const char* debug_name );
    rhi_shader_t  ( *shader_load_memory )( const void* spirv, u32 size, rhi_shader_stage_t stage,
                                            const char* entry, const char* debug_name );

    /* ---- Pipeline ---- */

    rhi_pipeline_t ( *pipeline_create         )( const rhi_pipeline_desc_t*         desc );
    rhi_pipeline_t ( *compute_pipeline_create )( const rhi_compute_pipeline_desc_t* desc );
    void           ( *pipeline_destroy        )( rhi_pipeline_t pipeline );

    /* ---- Staged upload (GPU_ONLY targets) ---- */

    /* Enqueues a copy via internal staging. The upload is flushed at the frame_begin
       that reuses this slot (VK_MAX_FRAMES_IN_FLIGHT frames later). The destination
       resource is safe to sample starting in that same frame; drawing from it any
       earlier reads VK_IMAGE_LAYOUT_UNDEFINED / stale buffer data. */
    bool ( *upload_buffer  )( rhi_buffer_t  dst, const void* data, u32 size );
    bool ( *upload_texture )( rhi_texture_t dst, const void* data, u32 data_size,
                              u16 mip, u16 layer );

    /* ---- Render pass ---- */

    /* Open a dynamic rendering pass.  Must be closed with cmd_end_rendering before
       frame_end or before opening another pass. */
    void ( *cmd_begin_rendering )( rhi_command_list_t          cmd,
                                   const rhi_color_attachment_t* color_atts, u32 color_count,
                                   const rhi_depth_attachment_t* depth_att );
    void ( *cmd_end_rendering   )( rhi_command_list_t cmd );

    /* ---- Commands ---- */

    void ( *cmd_set_viewport       )( rhi_command_list_t cmd, const rhi_viewport_t* vp );
    void ( *cmd_set_scissor        )( rhi_command_list_t cmd, const rhi_rect_t* rect );
    void ( *cmd_bind_pipeline      )( rhi_command_list_t cmd, rhi_pipeline_t pipeline );
    void ( *cmd_bind_vertex_buffer )( rhi_command_list_t cmd, rhi_buffer_t buf, u32 offset );
    void ( *cmd_bind_index_buffer  )( rhi_command_list_t cmd, rhi_buffer_t buf, u32 offset,
                                      rhi_index_type_t type );
    void ( *cmd_push_constants     )( rhi_command_list_t cmd, const void* data, u32 size,
                                      u32 offset );
    void ( *cmd_draw               )( rhi_command_list_t cmd, const rhi_draw_args_t* args );
    void ( *cmd_draw_indexed       )( rhi_command_list_t cmd,
                                      const rhi_draw_indexed_args_t* args );

    /* Indirect draw: draw parameters read from a GPU buffer (one VkDrawIndirectCommand per draw).
       buf must have RHI_BUFFER_USAGE_INDIRECT set.  stride is typically sizeof(VkDrawIndirectCommand). */
    void ( *cmd_draw_indirect         )( rhi_command_list_t cmd, rhi_buffer_t buf,
                                         u32 offset, u32 draw_count, u32 stride );
    void ( *cmd_draw_indexed_indirect )( rhi_command_list_t cmd, rhi_buffer_t buf,
                                         u32 offset, u32 draw_count, u32 stride );

    /* Compute dispatch: launch (groups_x * groups_y * groups_z) workgroups.
       A compute pipeline must be bound before calling this. */
    void ( *cmd_dispatch )( rhi_command_list_t cmd, u32 groups_x, u32 groups_y, u32 groups_z );

    /* ---- Bindless resource registration ---- */

    /* Returns a persistent slot index for use in shader push-constant lookups.  0 = invalid. */
    u32  ( *register_texture   )( rhi_texture_t tex );
    void ( *unregister_texture )( u32 bindless_index );
    u32  ( *register_sampler   )( rhi_sampler_t samp );
    void ( *unregister_sampler )( u32 bindless_index );

    /* Binds the global bindless descriptor set; call once at the top of each frame. */
    void ( *cmd_bind_bindless  )( rhi_command_list_t cmd );

    /* ---- Debug GPU labels (no-ops in release; always safe to call) ---- */

    void ( *cmd_begin_label )( rhi_command_list_t cmd, const char* name, f32 r, f32 g, f32 b );
    void ( *cmd_end_label   )( rhi_command_list_t cmd );

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
