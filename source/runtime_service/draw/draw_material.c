/*==============================================================================================

    runtime_service/draw/draw_material.c -- Pipeline creation.  SPIR-V lives in the generated
    draw_shader.h (compiled from shaders/*.{vert,frag}); this file only builds the pipelines.

    One material = one compiled rhi_pipeline_t.  All materials share one vertex buffer and
    stride (draw_vertex_t, 36 bytes):
        location 0 : float3 pos    (offset 0)
        location 1 : float4 color  (offset 12)  -- tint on the textured path
        location 2 : float2 uv     (offset 28)  -- consumed only by DRAW_MAT_TEXTURED
        color format : RHI_FORMAT_BGRA8_SRGB     (preferred Windows swapchain format)

    SOLID / SOLID_DEPTH read locations 0 + 1 and push draw_push_t (64 B, mvp only); the
    solid pipelines simply omit the uv attribute (attrib_count = 2).  TEXTURED reads all
    three, pushes draw_push_tex_t (72 B, mvp + bindless indices), and alpha-blends.

==============================================================================================*/

typedef struct
{
    rhi_pipeline_t pipeline;

} draw_material_t;

/*==============================================================================================
    Embedded SPIR-V for all draw pipelines (solid + textured), generated from
    shaders/*.{vert,frag}.  See draw_shader.h for the glslc recipe and the exact GLSL.
==============================================================================================*/

#include "runtime_service/draw/draw_shader.h"

/*==============================================================================================
    draw_material_init / draw_material_shutdown
==============================================================================================*/

static bool
draw_material_init( draw_material_t mats[ DRAW_MAT_COUNT ] )
{
    /* Compile shaders from embedded SPIR-V (draw_shader.h). */
    rhi_shader_t vert = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_solid_vert_spirv,
        .spirv_size = sizeof( s_solid_vert_spirv ),
        .stage      = RHI_SHADER_STAGE_VERTEX,
        .entry      = "main",
        .debug_name = "draw_solid_vert",
    } );
    if ( !rhi_handle_valid( vert ) )
        return false;

    rhi_shader_t frag = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_solid_frag_spirv,
        .spirv_size = sizeof( s_solid_frag_spirv ),
        .stage      = RHI_SHADER_STAGE_FRAGMENT,
        .entry      = "main",
        .debug_name = "draw_solid_frag",
    } );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
        return false;
    }

    /* Shared vertex layout, 36-byte stride: float3 pos @ loc 0, float4 color @ loc 1,
       float2 uv @ loc 2.  Solid pipelines bind only the first two attributes (they never
       read uv); the textured pipeline binds all three. */
    rhi_vertex_attrib_t attribs[ 3 ] = {
        { .binding = 0, .location = 0, .offset = 0,  .format = RHI_VERTEX_FORMAT_FLOAT3 },
        { .binding = 0, .location = 1, .offset = 12, .format = RHI_VERTEX_FORMAT_FLOAT4 },
        { .binding = 0, .location = 2, .offset = 28, .format = RHI_VERTEX_FORMAT_FLOAT2 },
    };

    /* Both pipelines share the same shaders, vertex layout, and color target; they differ
       only in depth state.  SOLID is the 2D/overlay path (no depth, draws on top); SOLID_DEPTH
       is the 3D path (depth test + write) and requires the caller to bind a DRAW_DEPTH_FORMAT
       depth attachment. */
    rhi_color_target_t color_target = { .format = RHI_FORMAT_BGRA8_SRGB };

    mats[ DRAW_MAT_SOLID ].pipeline = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vert,
        .frag               = frag,
        .attribs            = { attribs[ 0 ], attribs[ 1 ] },
        .attrib_count       = 2,
        .vertex_stride      = sizeof( draw_vertex_t ),
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( draw_push_t ),
        .debug_name         = "draw_solid",
    } );

    mats[ DRAW_MAT_SOLID_DEPTH ].pipeline = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vert,
        .frag               = frag,
        .attribs            = { attribs[ 0 ], attribs[ 1 ] },
        .attrib_count       = 2,
        .vertex_stride      = sizeof( draw_vertex_t ),
        .cull               = RHI_CULL_NONE,      /* depth handles occlusion; winding-independent */
        .depth_test         = true,
        .depth_write        = true,
        .depth_compare      = RHI_COMPARE_LESS,   /* zero-init would be NEVER -> rejects everything */
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = DRAW_DEPTH_FORMAT,
        .push_const_size    = sizeof( draw_push_t ),
        .debug_name         = "draw_solid_depth",
    } );

    /* Textured pipeline: separate shaders (bindless sampler), all three vertex attributes,
       and straight-alpha blending so images composite over the scene.  Push constants are
       draw_push_tex_t (mvp + bindless indices), 72 bytes -- still inside the 128-byte layout. */
    rhi_shader_t tex_vert = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_tex_vert_spirv,
        .spirv_size = sizeof( s_tex_vert_spirv ),
        .stage      = RHI_SHADER_STAGE_VERTEX,
        .entry      = "main",
        .debug_name = "draw_tex_vert",
    } );
    rhi_shader_t tex_frag = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_tex_frag_spirv,
        .spirv_size = sizeof( s_tex_frag_spirv ),
        .stage      = RHI_SHADER_STAGE_FRAGMENT,
        .entry      = "main",
        .debug_name = "draw_tex_frag",
    } );
    if ( rhi_handle_valid( tex_vert ) && rhi_handle_valid( tex_frag ) )
    {
        rhi_color_target_t blended = {
            .format       = RHI_FORMAT_BGRA8_SRGB,
            .blend_enable = true,
            .src_color = RHI_BLEND_SRC_ALPHA, .dst_color = RHI_BLEND_ONE_MINUS_SRC_A,
            .color_op  = RHI_BLEND_OP_ADD,
            .src_alpha = RHI_BLEND_ONE,       .dst_alpha = RHI_BLEND_ONE_MINUS_SRC_A,
            .alpha_op  = RHI_BLEND_OP_ADD,
        };

        mats[ DRAW_MAT_TEXTURED ].pipeline = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
            .vert               = tex_vert,
            .frag               = tex_frag,
            .attribs            = { attribs[ 0 ], attribs[ 1 ], attribs[ 2 ] },
            .attrib_count       = 3,
            .vertex_stride      = sizeof( draw_vertex_t ),
            .cull               = RHI_CULL_NONE,
            .depth_test         = false,
            .depth_write        = false,
            .color_targets      = { blended },
            .color_target_count = 1,
            .depth_format       = RHI_FORMAT_UNKNOWN,
            .push_const_size    = sizeof( draw_push_tex_t ),
            .debug_name         = "draw_textured",
        } );
    }
    if ( rhi_handle_valid( tex_frag ) ) rhi()->shader_destroy( tex_frag );
    if ( rhi_handle_valid( tex_vert ) ) rhi()->shader_destroy( tex_vert );

    /* Shaders are only needed during pipeline creation; free immediately after. */
    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );

    if ( !rhi_handle_valid( mats[ DRAW_MAT_SOLID ].pipeline ) )
        return false;
    if ( !rhi_handle_valid( mats[ DRAW_MAT_SOLID_DEPTH ].pipeline ) )
        return false;
    if ( !rhi_handle_valid( mats[ DRAW_MAT_TEXTURED ].pipeline ) )
        return false;

    return true;
}

static void
draw_material_shutdown( draw_material_t mats[ DRAW_MAT_COUNT ] )
{
    for ( u32 i = 0; i < DRAW_MAT_COUNT; ++i )
    {
        if ( rhi_handle_valid( mats[ i ].pipeline ) )
        {
            rhi()->pipeline_destroy( mats[ i ].pipeline );
            mats[ i ].pipeline = ( rhi_pipeline_t ){ 0 };
        }
    }
}

/*============================================================================================*/