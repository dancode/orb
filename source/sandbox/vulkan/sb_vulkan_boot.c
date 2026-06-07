/*==============================================================================================

    sandbox/vulkan/sb_vulkan_boot.c -- Bootstrap triangle render pass.

==============================================================================================*/

#include <stdio.h>

#include "sb_vulkan_boot.h"
#include "runtime_service/rhi/rhi_host.h"

/*==============================================================================================
    Vertex shader SPIR-V  (manually assembled, SPIR-V 1.3)

    Equivalent GLSL (version 450):
        void main() {
            const vec2 pos[3] = vec2[]( vec2(0.0,-0.5), vec2(0.5,0.5), vec2(-0.5,0.5) );
            gl_Position = vec4( pos[gl_VertexIndex], 0.0, 1.0 );
        }

    ID map:
        %1  = void  %2  = void()  %3  = float  %4  = vec4  %5  = vec2
        %6  = i32   %7  = u32     %8  = u32(3)  %9  = vec2[3]  %10 = gl_PerVertex{vec4}
        %11 = ptr(Out,PerVertex)  %12 = ptr(Fn,vec2[3])  %13 = ptr(Fn,vec2)
        %14 = ptr(Out,vec4)       %15 = ptr(In,i32)
        %16 = gl_PerVertex (Out)  %17 = gl_VertexIndex (In)
        %18..%22 = float/int constants
        %23..%26 = vec2/array composite constants
        %27 = main()   %28..%36 = function body temporaries
==============================================================================================*/

/* clang-format off */
static const u32 s_vert_spirv[] =
{
    /* Header: magic, version 1.3, generator 0, bound 37, schema 0 */
    0x07230203, 0x00010300, 0x00000000, 0x00000025, 0x00000000,
    /* OpCapability Shader */
    0x00020011, 0x00000001,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000E, 0x00000000, 0x00000001,
    /* OpEntryPoint Vertex %27 "main" %16 %17 */
    0x0007000F, 0x00000000, 0x0000001B, 0x6E69616D, 0x00000000, 0x00000010, 0x00000011,
    /* OpDecorate %10 Block */
    0x00030047, 0x0000000A, 0x00000002,
    /* OpMemberDecorate %10 0 BuiltIn Position(0) */
    0x00050048, 0x0000000A, 0x00000000, 0x0000000B, 0x00000000,
    /* OpDecorate %17 BuiltIn VertexIndex(42) */
    0x00040047, 0x00000011, 0x0000000B, 0x0000002A,
    /* %1 = OpTypeVoid */
    0x00020013, 0x00000001,
    /* %3 = OpTypeFloat 32 */
    0x00030016, 0x00000003, 0x00000020,
    /* %4 = OpTypeVector %3 4 */
    0x00040017, 0x00000004, 0x00000003, 0x00000004,
    /* %5 = OpTypeVector %3 2 */
    0x00040017, 0x00000005, 0x00000003, 0x00000002,
    /* %6 = OpTypeInt 32 1 (signed, for gl_VertexIndex) */
    0x00040015, 0x00000006, 0x00000020, 0x00000001,
    /* %7 = OpTypeInt 32 0 (unsigned, for array size constant) */
    0x00040015, 0x00000007, 0x00000020, 0x00000000,
    /* %8 = OpConstant %7 3 */
    0x0004002B, 0x00000007, 0x00000008, 0x00000003,
    /* %9 = OpTypeArray %5 %8  (vec2[3]) */
    0x0004001C, 0x00000009, 0x00000005, 0x00000008,
    /* %10 = OpTypeStruct %4  (gl_PerVertex: one vec4 member = gl_Position) */
    0x0003001E, 0x0000000A, 0x00000004,
    /* %2 = OpTypeFunction %1 */
    0x00030021, 0x00000002, 0x00000001,
    /* %11 = OpTypePointer Output %10 */
    0x00040020, 0x0000000B, 0x00000003, 0x0000000A,
    /* %12 = OpTypePointer Function %9 */
    0x00040020, 0x0000000C, 0x00000007, 0x00000009,
    /* %13 = OpTypePointer Function %5 */
    0x00040020, 0x0000000D, 0x00000007, 0x00000005,
    /* %14 = OpTypePointer Output %4 */
    0x00040020, 0x0000000E, 0x00000003, 0x00000004,
    /* %15 = OpTypePointer Input %6 */
    0x00040020, 0x0000000F, 0x00000001, 0x00000006,
    /* %16 = OpVariable %11 Output  (gl_PerVertex block) */
    0x0004003B, 0x0000000B, 0x00000010, 0x00000003,
    /* %17 = OpVariable %15 Input   (gl_VertexIndex) */
    0x0004003B, 0x0000000F, 0x00000011, 0x00000001,
    /* %18 = OpConstant %3 0.0f */
    0x0004002B, 0x00000003, 0x00000012, 0x00000000,
    /* %19 = OpConstant %3 -0.5f */
    0x0004002B, 0x00000003, 0x00000013, 0xBF000000,
    /* %20 = OpConstant %3  0.5f */
    0x0004002B, 0x00000003, 0x00000014, 0x3F000000,
    /* %21 = OpConstant %3  1.0f */
    0x0004002B, 0x00000003, 0x00000015, 0x3F800000,
    /* %22 = OpConstant %6  0  (i32 zero, member index for gl_Position) */
    0x0004002B, 0x00000006, 0x00000016, 0x00000000,
    /* %23 = OpConstantComposite %5 %18 %19  -> vec2( 0.0, -0.5) */
    0x0005002C, 0x00000005, 0x00000017, 0x00000012, 0x00000013,
    /* %24 = OpConstantComposite %5 %20 %20  -> vec2( 0.5,  0.5) */
    0x0005002C, 0x00000005, 0x00000018, 0x00000014, 0x00000014,
    /* %25 = OpConstantComposite %5 %19 %20  -> vec2(-0.5,  0.5) */
    0x0005002C, 0x00000005, 0x00000019, 0x00000013, 0x00000014,
    /* %26 = OpConstantComposite %9 %23 %24 %25  -> positions[3] */
    0x0006002C, 0x00000009, 0x0000001A, 0x00000017, 0x00000018, 0x00000019,
    /* %27 = OpFunction %1 None %2 */
    0x00050036, 0x00000001, 0x0000001B, 0x00000000, 0x00000002,
    /* %28 = OpLabel */
    0x000200F8, 0x0000001C,
    /* %29 = OpVariable %12 Function  (local copy of positions[3]) */
    0x0004003B, 0x0000000C, 0x0000001D, 0x00000007,
    /* OpStore %29 %26 */
    0x0003003E, 0x0000001D, 0x0000001A,
    /* %30 = OpLoad %6 %17  (load gl_VertexIndex) */
    0x0004003D, 0x00000006, 0x0000001E, 0x00000011,
    /* %31 = OpAccessChain %13 %29 %30  (ptr to positions[gl_VertexIndex]) */
    0x00050041, 0x0000000D, 0x0000001F, 0x0000001D, 0x0000001E,
    /* %32 = OpLoad %5 %31 */
    0x0004003D, 0x00000005, 0x00000020, 0x0000001F,
    /* %33 = OpCompositeExtract %3 %32 0  (x) */
    0x00050051, 0x00000003, 0x00000021, 0x00000020, 0x00000000,
    /* %34 = OpCompositeExtract %3 %32 1  (y) */
    0x00050051, 0x00000003, 0x00000022, 0x00000020, 0x00000001,
    /* %35 = OpCompositeConstruct %4 %33 %34 %18 %21  -> vec4(x, y, 0.0, 1.0) */
    0x00070050, 0x00000004, 0x00000023, 0x00000021, 0x00000022, 0x00000012, 0x00000015,
    /* %36 = OpAccessChain %14 %16 %22  (ptr to gl_Position in gl_PerVertex) */
    0x00050041, 0x0000000E, 0x00000024, 0x00000010, 0x00000016,
    /* OpStore %36 %35 */
    0x0003003E, 0x00000024, 0x00000023,
    /* OpReturn */
    0x000100FD,
    /* OpFunctionEnd */
    0x00010038,
};

/*==============================================================================================
    Fragment shader SPIR-V  (manually assembled, SPIR-V 1.3)

    Equivalent GLSL (version 450):
        layout(location = 0) out vec4 frag_color;
        void main() { frag_color = vec4( 1.0, 0.5, 0.2, 1.0 ); }

    ID map:
        %1 = void  %2 = void()  %3 = float  %4 = vec4
        %5 = ptr(Out,vec4)  %6 = frag_color (Out)
        %7 = main()  %8 = entry label
        %9..%11 = float constants (1.0, 0.5, 0.2)  %12 = composite (1,0.5,0.2,1)
==============================================================================================*/

static const u32 s_frag_spirv[] =
{
    /* Header: magic, version 1.3, generator 0, bound 13 (IDs 1..12), schema 0 */
    0x07230203, 0x00010300, 0x00000000, 0x0000000D, 0x00000000,
    /* OpCapability Shader */
    0x00020011, 0x00000001,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000E, 0x00000000, 0x00000001,
    /* OpEntryPoint Fragment %7 "main" %6 */
    0x0006000F, 0x00000004, 0x00000007, 0x6E69616D, 0x00000000, 0x00000006,
    /* OpExecutionMode %7 OriginUpperLeft(7) */
    0x00030010, 0x00000007, 0x00000007,
    /* OpDecorate %6 Location 0 */
    0x00040047, 0x00000006, 0x0000001E, 0x00000000,
    /* %1 = OpTypeVoid */
    0x00020013, 0x00000001,
    /* %2 = OpTypeFunction %1 */
    0x00030021, 0x00000002, 0x00000001,
    /* %3 = OpTypeFloat 32 */
    0x00030016, 0x00000003, 0x00000020,
    /* %4 = OpTypeVector %3 4 */
    0x00040017, 0x00000004, 0x00000003, 0x00000004,
    /* %5 = OpTypePointer Output %4 */
    0x00040020, 0x00000005, 0x00000003, 0x00000004,
    /* %6 = OpVariable %5 Output  (frag_color) */
    0x0004003B, 0x00000005, 0x00000006, 0x00000003,
    /* %9  = OpConstant %3 1.0f */
    0x0004002B, 0x00000003, 0x00000009, 0x3F800000,
    /* %10 = OpConstant %3 0.5f */
    0x0004002B, 0x00000003, 0x0000000A, 0x3F000000,
    /* %11 = OpConstant %3 0.2f */
    0x0004002B, 0x00000003, 0x0000000B, 0x3E4CCCCD,
    /* %12 = OpConstantComposite %4 %9 %10 %11 %9  -> vec4(1.0, 0.5, 0.2, 1.0) */
    0x0007002C, 0x00000004, 0x0000000C, 0x00000009, 0x0000000A, 0x0000000B, 0x00000009,
    /* %7 = OpFunction %1 None %2 */
    0x00050036, 0x00000001, 0x00000007, 0x00000000, 0x00000002,
    /* %8 = OpLabel */
    0x000200F8, 0x00000008,
    /* OpStore %6 %12 */
    0x0003003E, 0x00000006, 0x0000000C,
    /* OpReturn */
    0x000100FD,
    /* OpFunctionEnd */
    0x00010038,
};
/* clang-format on */

/*==============================================================================================
    sb_vk_boot_create
==============================================================================================*/

bool
sb_vk_boot_create( sb_vk_boot_t* boot )
{
    *boot = ( sb_vk_boot_t ){ 0 };

    boot->vert = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_vert_spirv,
        .spirv_size = sizeof( s_vert_spirv ),
        .stage      = RHI_SHADER_STAGE_VERTEX,
        .entry      = "main",
        .debug_name = "tri_vert",
    } );
    if ( !rhi_handle_valid( boot->vert ) )
    {
        fprintf( stderr, "[sb_vk_boot] shader_create(vert) failed\n" );
        return false;
    }

    boot->frag = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_frag_spirv,
        .spirv_size = sizeof( s_frag_spirv ),
        .stage      = RHI_SHADER_STAGE_FRAGMENT,
        .entry      = "main",
        .debug_name = "tri_frag",
    } );
    if ( !rhi_handle_valid( boot->frag ) )
    {
        fprintf( stderr, "[sb_vk_boot] shader_create(frag) failed\n" );
        rhi()->shader_destroy( boot->vert );
        boot->vert = ( rhi_shader_t ){ 0 };
        return false;
    }

    /* Color format matches swapchain preference (BGRA8_SRGB on most Windows GPUs).
       No vertex buffers; positions come from gl_VertexIndex in the vertex shader.
       No depth attachment; this is a 2D overlay draw. */
    rhi_color_target_t color_target = { .format = RHI_FORMAT_BGRA8_SRGB };
    boot->pipeline                  = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = boot->vert,
        .frag               = boot->frag,
        .attrib_count       = 0,
        .vertex_stride      = 0,
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = 0,
        .debug_name         = "tri_pipeline",
    } );
    if ( !rhi_handle_valid( boot->pipeline ) )
    {
        fprintf( stderr, "[sb_vk_boot] pipeline_create failed\n" );
        rhi()->shader_destroy( boot->frag );
        rhi()->shader_destroy( boot->vert );
        boot->frag = ( rhi_shader_t ){ 0 };
        boot->vert = ( rhi_shader_t ){ 0 };
        return false;
    }

    return true;
}

/*==============================================================================================
    sb_vk_boot_render
==============================================================================================*/

void
sb_vk_boot_render( sb_vk_boot_t* boot, rhi_cmd_list_t cmd, i32 win_w, i32 win_h )
{
    /* Bind global bindless set before any draws. */
    rhi()->cmd_bind_bindless( cmd );

    /* Begin render pass -- clear to dark green, no depth attachment. */
    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { 0.05f, 0.15f, 0.05f, 1.0f },
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    rhi_viewport_t vp = {
        .x         = 0.0f,
        .y         = 0.0f,
        .width     = (f32)win_w,
        .height    = (f32)win_h,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    };
    rhi_rect_t scissor = { .x = 0, .y = 0, .width = win_w, .height = win_h };
    rhi()->cmd_set_viewport( cmd, &vp );
    rhi()->cmd_set_scissor( cmd, &scissor );
    rhi()->cmd_bind_pipeline( cmd, boot->pipeline );
    rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){ .vertex_count = 3, .instance_count = 1 } );

    rhi()->cmd_end_rendering( cmd );
}

/*==============================================================================================
    sb_vk_boot_destroy
==============================================================================================*/

void
sb_vk_boot_destroy( sb_vk_boot_t* boot )
{
    if ( rhi_handle_valid( boot->pipeline ) )
        rhi()->pipeline_destroy( boot->pipeline );
    if ( rhi_handle_valid( boot->frag ) )
        rhi()->shader_destroy( boot->frag );
    if ( rhi_handle_valid( boot->vert ) )
        rhi()->shader_destroy( boot->vert );
    *boot = ( sb_vk_boot_t ){ 0 };
}

/*============================================================================================*/
