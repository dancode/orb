/*==============================================================================================

    sandbox/vulkan/sb_vulkan_boot.c -- Bootstrap triangle render pass.

==============================================================================================*/

#include <stdio.h>

#include "sb_vulkan_boot.h"
#include "runtime_service/rhi/rhi_host.h"

// clang-format off
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

    TEACHING: How to create GPU resources through the RHI.

    All GPU resources (shaders, pipelines, buffers, textures) are referenced through typed
    opaque handles -- rhi_shader_t, rhi_pipeline_t, etc.  Each is a struct wrapping a i32
    slot index.  Zero-initializing the struct always gives a safe null state.  Never compare
    .id directly; use rhi_handle_valid() for validity checks.

    Resource creation follows a descriptor pattern:
        1. Fill a stack-allocated desc struct with all parameters.
        2. Pass a pointer to the relevant rhi()->resource_create() function.
        3. The RHI copies everything it needs; the desc can be discarded after the call.
        4. Check rhi_handle_valid() on the returned handle.  An invalid handle (id == 0)
           means creation failed; the RHI will have logged details internally.
    
    Shaders are created from raw SPIR-V bytecode.  The RHI wraps the bytecode in a
    VkShaderModule but does not otherwise interpret it.  Shader objects are inputs to
    pipeline_create() and can be destroyed immediately after the pipeline is built --
    the pipeline keeps its own compiled reference.

    Pipelines encode all fixed-function state the GPU needs for a draw call.  Unlike raw
    Vulkan there are no separate VkRenderPass objects; the RHI uses dynamic rendering
    (VK_KHR_dynamic_rendering), so format compatibility is declared in the pipeline desc
    via color_targets[] and depth_format instead.  The formats here must match the actual
    attachments used when cmd_begin_rendering() is called at draw time.

    attrib_count = 0 because this pipeline reads no vertex buffers -- all three triangle
    positions are hard-coded in the vertex shader via gl_VertexIndex.
==============================================================================================*/

bool
sb_vk_boot_create( sb_vk_boot_t* boot )
{
    *boot = ( sb_vk_boot_t ){ 0 };

    /* Each shader stage is a separate object.  Both must succeed before the pipeline
       can be created.  On failure we clean up whichever stages already succeeded. */
    boot->vert = rhi()->shader_create( &( rhi_shader_desc_t ) {
        .spirv      = s_vert_spirv,
        .spirv_size = sizeof( s_vert_spirv ),
        .stage      = RHI_SHADER_STAGE_VERTEX,
        .entry      = "main",
        .debug_name = "tri_vert",     /* visible in Vulkan validation layers and GPU profilers */
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
        rhi()->shader_destroy( boot->vert );     /* vert succeeded; must be cleaned up */
        boot->vert = ( rhi_shader_t ){ 0 };
        return false;
    }

    /* color_targets tells the pipeline what format(s) it will write into.  With dynamic
       rendering this is a compatibility contract, not a render pass object; Vulkan validates
       it at draw time against whatever attachment was passed to cmd_begin_rendering().
       BGRA8_SRGB matches the swapchain format selected on most Windows GPUs. */
    rhi_color_target_t color_target = { .format = RHI_FORMAT_BGRA8_SRGB };

    boot->pipeline          = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = boot->vert,
        .frag               = boot->frag,
        .attrib_count       = 0,           /* no vertex buffer inputs; shader uses gl_VertexIndex */
        .vertex_stride      = 0,
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,       /* no depth attachment; 2D overlay draw */
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
        rhi()->shader_destroy( boot->frag );    /* pipeline_create does not own the shaders; */
        rhi()->shader_destroy( boot->vert );    /* we must destroy them on failure ourselves. */
        boot->frag = ( rhi_shader_t ){ 0 };
        boot->vert = ( rhi_shader_t ){ 0 };
        return false;
    }

    return true;
}

/*==============================================================================================
    sb_vk_boot_render

    TEACHING: The per-frame render sequence using the RHI command API.

    'cmd' is an opaque pointer into the current frame's command list.  It is obtained from
    rhi()->frame_begin() in the main loop and is only valid until rhi()->frame_end() is
    called.  Never cache it across frames.

    The minimal recording sequence for a draw call is:

        cmd_bind_bindless()         -- establish the global descriptor set (once per frame)
        cmd_begin_rendering()       -- open a dynamic render pass with attachment descs
        cmd_set_viewport()          -- required every frame; viewport is always dynamic state
        cmd_set_scissor()           -- required every frame; scissor is always dynamic state
        cmd_bind_pipeline()         -- select the PSO (shaders + fixed-function state)
        cmd_draw()                  -- emit the draw call
        cmd_end_rendering()         -- close the dynamic render pass

    Dynamic rendering (Vulkan 1.3 / VK_KHR_dynamic_rendering) means there are no VkRenderPass
    or VkFramebuffer objects.  Instead, cmd_begin_rendering() takes a list of attachment descs
    directly.  Each attachment specifies the texture to write into, what to do at load (clear
    vs. load the existing contents), and what to do at store (keep vs. discard).

    RHI_SWAPCHAIN_COLOR is a reserved magic handle ID.  Inside cmd_begin_rendering() the RHI
    resolves it to the actual swapchain image for the current frame.  This avoids exposing
    the swapchain image array to callers; the context knows which image index is active.

    Viewport and scissor are always dynamic in this RHI (VK_DYNAMIC_STATE_VIEWPORT and
    VK_DYNAMIC_STATE_SCISSOR are baked into every pipeline).  They must be recorded every
    frame even if the window size has not changed.
==============================================================================================*/

void
sb_vk_boot_render( sb_vk_boot_t* boot, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    /* Bind the global bindless descriptor set once at the top of the frame.  All GPU-resident
       buffers and textures are accessible to shaders via integer indices after this call.
       It does not need to be repeated after pipeline binds or draws: vkCmdBindDescriptorSets
       state survives vkCmdBindPipeline as long as the pipeline layout is compatible, and every
       pipeline in this RHI is compiled against the same global layout. */
    rhi()->cmd_bind_bindless( cmd );

    /* Open a dynamic render pass.  load_op CLEAR replaces any previous swapchain contents
       at the start of the pass -- no separate vkCmdClearColorImage call needed.
       store_op STORE preserves the result in the swapchain image for presentation. */
    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },  /* magic ID: resolves to current frame image */
        .load_op  = RHI_LOAD_OP_CLEAR, // RHI_LOAD_OP_DISCARD
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { 0.05f, 0.15f, 0.05f, 1.0f },  /* dark green background */
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );   /* NULL = no depth attachment */

    /* Viewport and scissor cover the full window.  min_depth/max_depth bracket the NDC
       depth range; 0..1 is the Vulkan convention (depth = 0 at near plane). */
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

    /* Binding the pipeline selects the compiled PSO (shaders + rasterizer + blend state).
       After this point the pipeline state is fixed until cmd_bind_pipeline() is called again
       or the render pass ends. */
    rhi()->cmd_bind_pipeline( cmd, boot->pipeline );

    /* Draw 3 vertices with no vertex buffer.  The vertex shader reads gl_VertexIndex (0,1,2)
       and computes positions from a hard-coded array; instance_count = 1 is required even
       for non-instanced draws. */
    rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){ .vertex_count = 3, .instance_count = 1 } );

    /* End the dynamic render pass.  The RHI inserts a pipeline barrier to transition the
       swapchain image from COLOR_ATTACHMENT_OPTIMAL to PRESENT_SRC_KHR before frame_end()
       queues it for display. */
    rhi()->cmd_end_rendering( cmd );
}

/*==============================================================================================
    sb_vk_boot_destroy

    TEACHING: Correct teardown order and the safe-to-call-on-partial-init pattern.

    GPU resources must be destroyed before the device.  The caller (sb_vulkan.c main) already
    ensures the GPU is idle via context_destroy() before this function runs, so no additional
    vkDeviceWaitIdle() is needed here.

    Shaders and pipelines are independent after pipeline_create() returns.  In Vulkan, the
    pipeline holds a compiled copy of the shader code; the VkShaderModule wrappers can be
    destroyed at any point after the pipeline is built.  Here we kept them alive alongside the
    pipeline for simplicity, but either order is valid at teardown.

    rhi_handle_valid() guards each destroy call to make this safe even if sb_vk_boot_create()
    failed partway through -- boot starts zeroed, so any handles that were never assigned
    will be invalid and the corresponding destroy call is skipped.

    Zeroing *boot after all destroys resets handle ids to 0 (RHI_NULL_HANDLE), preventing
    accidental double-free if destroy is called a second time.
==============================================================================================*/

void
sb_vk_boot_destroy( sb_vk_boot_t* boot )
{
    /* Destroy the pipeline before its source shaders; mirrors logical dependency order. */
    if ( rhi_handle_valid( boot->pipeline ) )
        rhi()->pipeline_destroy( boot->pipeline );
    if ( rhi_handle_valid( boot->frag ) )
        rhi()->shader_destroy( boot->frag );
    if ( rhi_handle_valid( boot->vert ) )
        rhi()->shader_destroy( boot->vert );

    /* Reset all handles to their null state so a second call is harmless. */
    *boot = ( sb_vk_boot_t ){ 0 };
}

/*============================================================================================*/
// clang-format on