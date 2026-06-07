/*==============================================================================================

    runtime_service/rhi/rhi.h -- Render Hardware Interface, public types and handles.

    Targets Vulkan 1.3 (dynamic rendering, synchronization2, descriptor indexing).

    All GPU resources are referenced through opaque slot handles (u32):
        handle.id is the pool slot index; slot 0 is permanently reserved as invalid
        id == 0 (RHI_NULL_HANDLE) is always invalid regardless of type
        Handles carry no generation counter -- they are stable for the resource lifetime

    Three-stage init (host-driven):
        1. rhi_mod_init              : loads vulkan-1.dll (cheap; no-op if absent)
        2. rhi()->init()             : VkInstance + VkDevice; no window needed
        3. rhi()->context_create()   : per-window surface + swapchain + sync + commands

==============================================================================================*/
#ifndef RHI_H
#define RHI_H

#include "orb.h"

/*==============================================================================================
    Resource handles

    Typed wrappers around u32.  Zero-initializing any handle gives a safe null state.
    rhi_handle_valid() is the only correct way to check for null -- do not compare .id directly
    against magic values other than RHI_NULL_HANDLE.
==============================================================================================*/

#define RHI_DEFINE_HANDLE( name ) typedef struct { u32 id; } name

RHI_DEFINE_HANDLE( rhi_buffer_t   );
RHI_DEFINE_HANDLE( rhi_texture_t  );
RHI_DEFINE_HANDLE( rhi_sampler_t  );
RHI_DEFINE_HANDLE( rhi_shader_t   );
RHI_DEFINE_HANDLE( rhi_pipeline_t );

#define RHI_NULL_HANDLE          0u
#define rhi_handle_valid( h )    ( (h).id != RHI_NULL_HANDLE )

/*==============================================================================================
    Command list handle  (opaque pointer into vk_context_t::cmd_lists[]; NULL = invalid)
==============================================================================================*/

struct rhi_cmd_list_s;
typedef struct rhi_cmd_list_s* rhi_cmd_list_t;

#define RHI_CMD_INVALID     NULL
#define rhi_cmd_valid(cmd)  ( (cmd) != NULL )

/*==============================================================================================
    Render context pool  (one per platform window)
==============================================================================================*/

#define RHI_CTX_INVALID  ( -1 )
#define RHI_CTX_MAX      4        /* must match APP_WIN_MAX */

/*==============================================================================================
    Pixel formats

    Engine-side enum.  Internal VkFormat mappings live in vk_texture.c.
==============================================================================================*/

typedef enum rhi_format_e
{
    RHI_FORMAT_UNKNOWN = 0,

    /* 8-bit normalized */
    RHI_FORMAT_RGBA8_UNORM,
    RHI_FORMAT_RGBA8_SRGB,
    RHI_FORMAT_BGRA8_UNORM,
    RHI_FORMAT_BGRA8_SRGB,

    /* HDR float */
    RHI_FORMAT_RGBA16_FLOAT,
    RHI_FORMAT_RG11B10_FLOAT,
    RHI_FORMAT_RGB9E5_FLOAT,

    /* Scalar */
    RHI_FORMAT_R8_UNORM,
    RHI_FORMAT_R16_FLOAT,
    RHI_FORMAT_R32_FLOAT,
    RHI_FORMAT_RG8_UNORM,
    RHI_FORMAT_RG16_FLOAT,
    RHI_FORMAT_RG32_FLOAT,

    /* Depth / stencil */
    RHI_FORMAT_D32_FLOAT,
    RHI_FORMAT_D24_UNORM_S8_UINT,
    RHI_FORMAT_D16_UNORM,

} rhi_format_t;

/*==============================================================================================
    Buffer
==============================================================================================*/

typedef enum rhi_buffer_usage_e
{
    RHI_BUFFER_USAGE_VERTEX       = BIT( 0 ),
    RHI_BUFFER_USAGE_INDEX        = BIT( 1 ),
    RHI_BUFFER_USAGE_UNIFORM      = BIT( 2 ),
    RHI_BUFFER_USAGE_STORAGE      = BIT( 3 ),
    RHI_BUFFER_USAGE_INDIRECT     = BIT( 4 ),
    RHI_BUFFER_USAGE_TRANSFER_SRC  = BIT( 5 ),
    RHI_BUFFER_USAGE_TRANSFER_DST  = BIT( 6 ),
    RHI_BUFFER_USAGE_DEVICE_ADDRESS = BIT( 7 ),   /* enables vkGetBufferDeviceAddress on this buffer */

} rhi_buffer_usage_t;

typedef enum rhi_memory_e
{
    RHI_MEMORY_GPU_ONLY   = 0,   /* device-local; fastest GPU access, no CPU visibility */
    RHI_MEMORY_CPU_TO_GPU = 1,   /* host-visible + coherent; per-frame streaming data */
    RHI_MEMORY_CPU_ONLY   = 2,   /* host-visible staging; transfer source only */

} rhi_memory_t;

typedef struct rhi_buffer_desc_s
{
    u32                size;
    rhi_buffer_usage_t usage;
    rhi_memory_t       memory;
    const char*        debug_name;   /* VK_EXT_debug_utils label; NULL is fine */

} rhi_buffer_desc_t;

/*==============================================================================================
    Texture
==============================================================================================*/

typedef enum rhi_texture_usage_e
{
    RHI_TEXTURE_USAGE_SAMPLED          = BIT( 0 ),
    RHI_TEXTURE_USAGE_STORAGE          = BIT( 1 ),
    RHI_TEXTURE_USAGE_COLOR_ATTACHMENT = BIT( 2 ),
    RHI_TEXTURE_USAGE_DEPTH_ATTACHMENT = BIT( 3 ),
    RHI_TEXTURE_USAGE_TRANSFER_SRC     = BIT( 4 ),
    RHI_TEXTURE_USAGE_TRANSFER_DST     = BIT( 5 ),

} rhi_texture_usage_t;

typedef struct rhi_texture_desc_s
{
    u32                 width;
    u32                 height;
    u16                 depth;          /* 1 = 2D, >1 = 3D */
    u16                 mip_levels;     /* 0 = auto-compute from width/height */
    u16                 array_layers;   /* 1 = non-array */
    rhi_format_t        format;
    rhi_texture_usage_t usage;
    rhi_memory_t        memory;
    const char*         debug_name;

} rhi_texture_desc_t;

/*==============================================================================================
    Sampler
==============================================================================================*/

typedef enum rhi_filter_e
{
    RHI_FILTER_NEAREST = 0,
    RHI_FILTER_LINEAR  = 1,

} rhi_filter_t;

typedef enum rhi_address_mode_e
{
    RHI_ADDRESS_MODE_REPEAT          = 0,
    RHI_ADDRESS_MODE_MIRRORED_REPEAT = 1,
    RHI_ADDRESS_MODE_CLAMP_TO_EDGE   = 2,
    RHI_ADDRESS_MODE_CLAMP_TO_BORDER = 3,

} rhi_address_mode_t;

typedef struct rhi_sampler_desc_s
{
    rhi_filter_t       min_filter;
    rhi_filter_t       mag_filter;
    rhi_filter_t       mip_filter;
    rhi_address_mode_t address_u;
    rhi_address_mode_t address_v;
    rhi_address_mode_t address_w;
    f32                max_anisotropy;   /* 0.0 = no anisotropy */
    f32                min_lod;
    f32                max_lod;

} rhi_sampler_desc_t;

/*==============================================================================================
    Shader
==============================================================================================*/

typedef enum rhi_shader_stage_e
{
    RHI_SHADER_STAGE_VERTEX   = BIT( 0 ),
    RHI_SHADER_STAGE_FRAGMENT = BIT( 1 ),
    RHI_SHADER_STAGE_COMPUTE  = BIT( 2 ),

} rhi_shader_stage_t;

typedef struct rhi_shader_desc_s
{
    const void*        spirv;        /* SPIR-V bytecode pointer */
    u32                spirv_size;   /* byte count */
    rhi_shader_stage_t stage;
    const char*        entry;        /* entry point name; "main" is conventional */
    const char*        debug_name;

} rhi_shader_desc_t;

/*==============================================================================================
    Pipeline (graphics)

    Uses dynamic rendering (VK 1.3): no VkRenderPass or VkFramebuffer.
    All pipelines share a common bindless VkPipelineLayout (push constants + bindless set).

    Compute pipelines use rhi_compute_pipeline_desc_t + compute_pipeline_create().
    The resulting rhi_pipeline_t handle is passed to cmd_bind_pipeline exactly like
    a graphics pipeline; the backend picks the correct VkPipelineBindPoint automatically.
==============================================================================================*/

typedef enum rhi_cull_mode_e
{
    RHI_CULL_NONE  = 0,
    RHI_CULL_FRONT = 1,
    RHI_CULL_BACK  = 2,

} rhi_cull_mode_t;

typedef enum rhi_blend_factor_e
{
    RHI_BLEND_ZERO             = 0,
    RHI_BLEND_ONE              = 1,
    RHI_BLEND_SRC_COLOR        = 2,
    RHI_BLEND_ONE_MINUS_SRC_C  = 3,
    RHI_BLEND_SRC_ALPHA        = 4,
    RHI_BLEND_ONE_MINUS_SRC_A  = 5,
    RHI_BLEND_DST_ALPHA        = 6,
    RHI_BLEND_ONE_MINUS_DST_A  = 7,

} rhi_blend_factor_t;

typedef enum rhi_blend_op_e
{
    RHI_BLEND_OP_ADD      = 0,
    RHI_BLEND_OP_SUBTRACT = 1,
    RHI_BLEND_OP_MIN      = 2,
    RHI_BLEND_OP_MAX      = 3,

} rhi_blend_op_t;

typedef enum rhi_compare_op_e
{
    RHI_COMPARE_NEVER         = 0,
    RHI_COMPARE_LESS          = 1,
    RHI_COMPARE_EQUAL         = 2,
    RHI_COMPARE_LESS_EQUAL    = 3,
    RHI_COMPARE_GREATER       = 4,
    RHI_COMPARE_NOT_EQUAL     = 5,
    RHI_COMPARE_GREATER_EQUAL = 6,
    RHI_COMPARE_ALWAYS        = 7,

} rhi_compare_op_t;

typedef enum rhi_vertex_format_e
{
    RHI_VERTEX_FORMAT_FLOAT   = 0,
    RHI_VERTEX_FORMAT_FLOAT2  = 1,
    RHI_VERTEX_FORMAT_FLOAT3  = 2,
    RHI_VERTEX_FORMAT_FLOAT4  = 3,
    RHI_VERTEX_FORMAT_UINT    = 4,
    RHI_VERTEX_FORMAT_UINT2   = 5,
    RHI_VERTEX_FORMAT_UINT4   = 6,
    RHI_VERTEX_FORMAT_UNORM4  = 7,   /* packed 8-bit RGBA normalized */

} rhi_vertex_format_t;

typedef enum rhi_index_type_e
{
    RHI_INDEX_TYPE_UINT16 = 0,
    RHI_INDEX_TYPE_UINT32 = 1,

} rhi_index_type_t;

#define RHI_MAX_VERTEX_ATTRIBS   8
#define RHI_MAX_COLOR_TARGETS    4
#define RHI_MAX_PUSH_CONST_SIZE  128   /* bytes; guaranteed minimum by the Vulkan spec */

typedef struct rhi_vertex_attrib_s
{
    u32                 binding;
    u32                 location;
    u32                 offset;
    rhi_vertex_format_t format;

} rhi_vertex_attrib_t;

typedef struct rhi_color_target_s
{
    rhi_format_t       format;
    bool               blend_enable;
    rhi_blend_factor_t src_color;
    rhi_blend_factor_t dst_color;
    rhi_blend_op_t     color_op;
    rhi_blend_factor_t src_alpha;
    rhi_blend_factor_t dst_alpha;
    rhi_blend_op_t     alpha_op;

} rhi_color_target_t;

typedef struct rhi_pipeline_desc_s
{
    rhi_shader_t  vert;
    rhi_shader_t  frag;

    /* Vertex input */

    rhi_vertex_attrib_t attribs[ RHI_MAX_VERTEX_ATTRIBS ];
    u32                 attrib_count;
    u32                 vertex_stride;       /* single interleaved binding for now */

    /* Rasterizer */

    rhi_cull_mode_t     cull;
    bool                depth_test;
    bool                depth_write;
    rhi_compare_op_t    depth_compare;      /* default: RHI_COMPARE_LESS */

    /* Dynamic rendering attachments (no VkRenderPass) */

    rhi_color_target_t  color_targets[ RHI_MAX_COLOR_TARGETS ];
    u32                 color_target_count;
    rhi_format_t        depth_format;       /* RHI_FORMAT_UNKNOWN = no depth attachment */

    /* Push constants (vert + frag share one range) */

    u32                 push_const_size;    /* bytes; 0 = none; max RHI_MAX_PUSH_CONST_SIZE */

    const char*         debug_name;

} rhi_pipeline_desc_t;

/*==============================================================================================
    Pipeline (compute)
==============================================================================================*/

typedef struct rhi_compute_pipeline_desc_s
{
    rhi_shader_t  comp;
    u32           push_const_size;   /* bytes; 0 = none; max RHI_MAX_PUSH_CONST_SIZE */
    const char*   debug_name;

} rhi_compute_pipeline_desc_t;

/*==============================================================================================
    Draw parameters
==============================================================================================*/

typedef struct rhi_viewport_s
{
    f32 x, y;
    f32 width, height;
    f32 min_depth;
    f32 max_depth;

} rhi_viewport_t;

typedef struct rhi_rect_s
{
    i32 x, y;
    i32 width, height;

} rhi_rect_t;

typedef struct rhi_draw_args_s
{
    u32 vertex_count;
    u32 instance_count;
    u32 first_vertex;
    u32 first_instance;

} rhi_draw_args_t;

typedef struct rhi_draw_indexed_args_s
{
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    i32 vertex_offset;
    u32 first_instance;

} rhi_draw_indexed_args_t;

/*==============================================================================================
    Value types
==============================================================================================*/

typedef struct rhi_color_s
{
    f32 r, g, b, a;

} rhi_color_t;

/*==============================================================================================
    Render pass attachments  (for cmd_begin_rendering / cmd_end_rendering)

    Sentinel IDs for context-owned attachments:
        RHI_SWAPCHAIN_COLOR -- current swapchain image (color attachment)
        RHI_SWAPCHAIN_DEPTH -- context depth buffer (depth attachment)
==============================================================================================*/

#define RHI_SWAPCHAIN_COLOR  0xFFFFFFFFu
#define RHI_SWAPCHAIN_DEPTH  0xFFFFFFFEu

typedef enum rhi_load_op_e
{
    RHI_LOAD_OP_LOAD    = 0,   /* preserve existing contents */
    RHI_LOAD_OP_CLEAR   = 1,   /* clear to the provided clear value */
    RHI_LOAD_OP_DISCARD = 2,   /* contents undefined; fastest */

} rhi_load_op_t;

typedef enum rhi_store_op_e
{
    RHI_STORE_OP_STORE   = 0,   /* written contents are preserved after the pass */
    RHI_STORE_OP_DISCARD = 1,   /* contents discarded; use for transient depth etc. */

} rhi_store_op_t;

typedef struct rhi_color_attachment_s
{
    rhi_texture_t  texture;    /* RHI_SWAPCHAIN_COLOR = current swapchain image */
    rhi_load_op_t  load_op;
    rhi_store_op_t store_op;
    rhi_color_t    clear;      /* used when load_op == RHI_LOAD_OP_CLEAR */

} rhi_color_attachment_t;

typedef struct rhi_depth_attachment_s
{
    rhi_texture_t  texture;       /* RHI_SWAPCHAIN_DEPTH = context depth buffer */
    rhi_load_op_t  load_op;
    rhi_store_op_t store_op;
    f32            depth_clear;   /* used when load_op == RHI_LOAD_OP_CLEAR */
    u32            stencil_clear;

} rhi_depth_attachment_t;

/*============================================================================================*/
#endif    // RHI_H
