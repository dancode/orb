/*==============================================================================================

    runtime_service/draw/draw_material.c -- Pipeline creation and embedded SPIR-V.

    One material = one compiled rhi_pipeline_t.  All materials share:
        vertex layout : float3 @ location 0 (pos), float4 @ location 1 (color)
        push constants: draw_push_t (64 bytes = column-major mvp)
        color format  : RHI_FORMAT_BGRA8_SRGB  (preferred Windows swapchain format)
        depth format  : RHI_FORMAT_UNKNOWN      (no depth; draws always on top)

==============================================================================================*/

typedef struct
{
    rhi_pipeline_t pipeline;

} draw_material_t;

/*==============================================================================================
    Vertex shader SPIR-V  (manually assembled, SPIR-V 1.3)

    Equivalent GLSL (version 450):
        layout(location = 0) in  vec3 in_pos;
        layout(location = 1) in  vec4 in_color;
        layout(push_constant) uniform PC { mat4 mvp; } pc;
        layout(location = 0) out vec4 v_color;
        void main() {
            gl_Position = pc.mvp * vec4( in_pos, 1.0 );
            v_color     = in_color;
        }

    ID map (bound = 35):
        %1 void  %2 void()  %3 float  %4 vec4  %5 vec3  %6 mat4
        %7 struct{mat4}(PC)  %8 i32  %9 struct{vec4}(gl_PerVertex)
        %10 ptr(PushConst,%7)  %11 ptr(In,%5)  %12 ptr(In,%4)
        %13 ptr(Out,%9)        %14 ptr(Out,%4)  %15 ptr(PushConst,%6)
        %16 pc  %17 in_pos  %18 in_color  %19 gl_PerVertex  %20 v_color
        %21 i32(0)  %22 float(1.0)
        %23 main()  %24 label  %25-%34 temporaries
==============================================================================================*/

/* clang-format off */
static const u32 s_vert_spirv[] =
{
    /* Header: magic, version 1.3, generator 0, bound 35, schema 0 */
    0x07230203, 0x00010300, 0x00000000, 0x00000023, 0x00000000,
    /* OpCapability Shader */
    0x00020011, 0x00000001,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000E, 0x00000000, 0x00000001,
    /* OpEntryPoint Vertex %23 "main" %17 %18 %19 %20 */
    0x0009000F, 0x00000000, 0x00000017, 0x6E69616D, 0x00000000,
               0x00000011, 0x00000012, 0x00000013, 0x00000014,
    /* OpDecorate %7 Block (PC struct) */
    0x00030047, 0x00000007, 0x00000002,
    /* OpMemberDecorate %7 0 Offset 0 */
    0x00050048, 0x00000007, 0x00000000, 0x00000023, 0x00000000,
    /* OpMemberDecorate %7 0 ColMajor */
    0x00040048, 0x00000007, 0x00000000, 0x00000005,
    /* OpMemberDecorate %7 0 MatrixStride 16 */
    0x00050048, 0x00000007, 0x00000000, 0x00000007, 0x00000010,
    /* OpDecorate %9 Block (gl_PerVertex struct) */
    0x00030047, 0x00000009, 0x00000002,
    /* OpMemberDecorate %9 0 BuiltIn Position(0) */
    0x00050048, 0x00000009, 0x00000000, 0x0000000B, 0x00000000,
    /* OpDecorate %17 Location 0 (in_pos) */
    0x00040047, 0x00000011, 0x0000001E, 0x00000000,
    /* OpDecorate %18 Location 1 (in_color) */
    0x00040047, 0x00000012, 0x0000001E, 0x00000001,
    /* OpDecorate %20 Location 0 (v_color) */
    0x00040047, 0x00000014, 0x0000001E, 0x00000000,
    /* %1  = OpTypeVoid */
    0x00020013, 0x00000001,
    /* %2  = OpTypeFunction %1 */
    0x00030021, 0x00000002, 0x00000001,
    /* %3  = OpTypeFloat 32 */
    0x00030016, 0x00000003, 0x00000020,
    /* %4  = OpTypeVector %3 4 (vec4) */
    0x00040017, 0x00000004, 0x00000003, 0x00000004,
    /* %5  = OpTypeVector %3 3 (vec3) */
    0x00040017, 0x00000005, 0x00000003, 0x00000003,
    /* %6  = OpTypeMatrix %4 4 (mat4: 4 columns of vec4) */
    0x00040018, 0x00000006, 0x00000004, 0x00000004,
    /* %7  = OpTypeStruct %6 (push constant block: {mat4 mvp}) */
    0x0003001E, 0x00000007, 0x00000006,
    /* %8  = OpTypeInt 32 1 (i32 signed) */
    0x00040015, 0x00000008, 0x00000020, 0x00000001,
    /* %9  = OpTypeStruct %4 (gl_PerVertex: {vec4 position}) */
    0x0003001E, 0x00000009, 0x00000004,
    /* %10 = OpTypePointer PushConstant %7 */
    0x00040020, 0x0000000A, 0x00000009, 0x00000007,
    /* %11 = OpTypePointer Input %5 */
    0x00040020, 0x0000000B, 0x00000001, 0x00000005,
    /* %12 = OpTypePointer Input %4 */
    0x00040020, 0x0000000C, 0x00000001, 0x00000004,
    /* %13 = OpTypePointer Output %9 */
    0x00040020, 0x0000000D, 0x00000003, 0x00000009,
    /* %14 = OpTypePointer Output %4 */
    0x00040020, 0x0000000E, 0x00000003, 0x00000004,
    /* %15 = OpTypePointer PushConstant %6 (ptr to mat4 member in PC) */
    0x00040020, 0x0000000F, 0x00000009, 0x00000006,
    /* %16 = OpVariable %10 PushConstant (pc) */
    0x0004003B, 0x0000000A, 0x00000010, 0x00000009,
    /* %17 = OpVariable %11 Input (in_pos) */
    0x0004003B, 0x0000000B, 0x00000011, 0x00000001,
    /* %18 = OpVariable %12 Input (in_color) */
    0x0004003B, 0x0000000C, 0x00000012, 0x00000001,
    /* %19 = OpVariable %13 Output (gl_PerVertex) */
    0x0004003B, 0x0000000D, 0x00000013, 0x00000003,
    /* %20 = OpVariable %14 Output (v_color) */
    0x0004003B, 0x0000000E, 0x00000014, 0x00000003,
    /* %21 = OpConstant %8 0 (i32 zero, member index) */
    0x0004002B, 0x00000008, 0x00000015, 0x00000000,
    /* %22 = OpConstant %3 1.0f (w component of position) */
    0x0004002B, 0x00000003, 0x00000016, 0x3F800000,
    /* %23 = OpFunction %1 None %2 */
    0x00050036, 0x00000001, 0x00000017, 0x00000000, 0x00000002,
    /* %24 = OpLabel */
    0x000200F8, 0x00000018,
    /* %25 = OpAccessChain %15 %16 %21  -> ptr to pc.mvp */
    0x00050041, 0x0000000F, 0x00000019, 0x00000010, 0x00000015,
    /* %26 = OpLoad %6 %25  -> mat4 mvp */
    0x0004003D, 0x00000006, 0x0000001A, 0x00000019,
    /* %27 = OpLoad %5 %17  -> vec3 in_pos */
    0x0004003D, 0x00000005, 0x0000001B, 0x00000011,
    /* %28 = OpCompositeExtract %3 %27 0  -> x */
    0x00050051, 0x00000003, 0x0000001C, 0x0000001B, 0x00000000,
    /* %29 = OpCompositeExtract %3 %27 1  -> y */
    0x00050051, 0x00000003, 0x0000001D, 0x0000001B, 0x00000001,
    /* %30 = OpCompositeExtract %3 %27 2  -> z */
    0x00050051, 0x00000003, 0x0000001E, 0x0000001B, 0x00000002,
    /* %31 = OpCompositeConstruct %4 %28 %29 %30 %22  -> vec4(in_pos, 1.0) */
    0x00070050, 0x00000004, 0x0000001F, 0x0000001C, 0x0000001D, 0x0000001E, 0x00000016,
    /* %32 = OpMatrixTimesVector %4 %26 %31  -> mvp * vec4(pos,1) */
    0x00050091, 0x00000004, 0x00000020, 0x0000001A, 0x0000001F,
    /* %33 = OpAccessChain %14 %19 %21  -> ptr to gl_Position */
    0x00050041, 0x0000000E, 0x00000021, 0x00000013, 0x00000015,
    /* OpStore %33 %32 */
    0x0003003E, 0x00000021, 0x00000020,
    /* %34 = OpLoad %4 %18  -> vec4 in_color */
    0x0004003D, 0x00000004, 0x00000022, 0x00000012,
    /* OpStore %20 %34 */
    0x0003003E, 0x00000014, 0x00000022,
    /* OpReturn */
    0x000100FD,
    /* OpFunctionEnd */
    0x00010038,
};

/*==============================================================================================
    Fragment shader SPIR-V  (manually assembled, SPIR-V 1.3)

    Equivalent GLSL (version 450):
        layout(location = 0) in  vec4 in_color;
        layout(location = 0) out vec4 out_color;
        void main() { out_color = in_color; }

    ID map (bound = 12):
        %1 void  %2 void()  %3 float  %4 vec4
        %5 ptr(In,%4)  %6 ptr(Out,%4)
        %7 in_color(In,loc0)  %8 out_color(Out,loc0)
        %9 main()  %10 label  %11 loaded color
==============================================================================================*/

static const u32 s_frag_spirv[] =
{
    /* Header: magic, version 1.3, generator 0, bound 12, schema 0 */
    0x07230203, 0x00010300, 0x00000000, 0x0000000C, 0x00000000,
    /* OpCapability Shader */
    0x00020011, 0x00000001,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000E, 0x00000000, 0x00000001,
    /* OpEntryPoint Fragment %9 "main" %7 %8 */
    0x0007000F, 0x00000004, 0x00000009, 0x6E69616D, 0x00000000, 0x00000007, 0x00000008,
    /* OpExecutionMode %9 OriginUpperLeft(7) */
    0x00030010, 0x00000009, 0x00000007,
    /* OpDecorate %7 Location 0 (in_color) */
    0x00040047, 0x00000007, 0x0000001E, 0x00000000,
    /* OpDecorate %8 Location 0 (out_color) */
    0x00040047, 0x00000008, 0x0000001E, 0x00000000,
    /* %1 = OpTypeVoid */
    0x00020013, 0x00000001,
    /* %2 = OpTypeFunction %1 */
    0x00030021, 0x00000002, 0x00000001,
    /* %3 = OpTypeFloat 32 */
    0x00030016, 0x00000003, 0x00000020,
    /* %4 = OpTypeVector %3 4 (vec4) */
    0x00040017, 0x00000004, 0x00000003, 0x00000004,
    /* %5 = OpTypePointer Input %4 */
    0x00040020, 0x00000005, 0x00000001, 0x00000004,
    /* %6 = OpTypePointer Output %4 */
    0x00040020, 0x00000006, 0x00000003, 0x00000004,
    /* %7 = OpVariable %5 Input (in_color, location 0) */
    0x0004003B, 0x00000005, 0x00000007, 0x00000001,
    /* %8 = OpVariable %6 Output (out_color, location 0) */
    0x0004003B, 0x00000006, 0x00000008, 0x00000003,
    /* %9 = OpFunction %1 None %2 */
    0x00050036, 0x00000001, 0x00000009, 0x00000000, 0x00000002,
    /* %10 = OpLabel */
    0x000200F8, 0x0000000A,
    /* %11 = OpLoad %4 %7 */
    0x0004003D, 0x00000004, 0x0000000B, 0x00000007,
    /* OpStore %8 %11 */
    0x0003003E, 0x00000008, 0x0000000B,
    /* OpReturn */
    0x000100FD,
    /* OpFunctionEnd */
    0x00010038,
};
/* clang-format on */

/*==============================================================================================
    draw_material_init / draw_material_shutdown
==============================================================================================*/

static bool
draw_material_init( draw_material_t mats[ DRAW_MAT_COUNT ] )
{
    /* Compile shaders from embedded SPIR-V. */
    rhi_shader_t vert = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_vert_spirv,
        .spirv_size = sizeof( s_vert_spirv ),
        .stage      = RHI_SHADER_STAGE_VERTEX,
        .entry      = "main",
        .debug_name = "draw_solid_vert",
    } );
    if ( !rhi_handle_valid( vert ) )
        return false;

    rhi_shader_t frag = rhi()->shader_create( &( rhi_shader_desc_t ){
        .spirv      = s_frag_spirv,
        .spirv_size = sizeof( s_frag_spirv ),
        .stage      = RHI_SHADER_STAGE_FRAGMENT,
        .entry      = "main",
        .debug_name = "draw_solid_frag",
    } );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
        return false;
    }

    /* Vertex layout: float3 pos @ loc 0, float4 color @ loc 1, 28-byte stride. */
    rhi_vertex_attrib_t attribs[ 2 ] = {
        { .binding = 0, .location = 0, .offset = 0,  .format = RHI_VERTEX_FORMAT_FLOAT3 },
        { .binding = 0, .location = 1, .offset = 12, .format = RHI_VERTEX_FORMAT_FLOAT4 },
    };

    /* One solid-color pipeline: no depth test, no culling, alpha blending off. */
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

    /* Shaders are only needed during pipeline creation; free immediately after. */
    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );

    if ( !rhi_handle_valid( mats[ DRAW_MAT_SOLID ].pipeline ) )
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