/*==============================================================================================

    vulkan/vk_pipeline.c -- Graphics PSO creation with a persistent cache.

    Slot pool: vk.pipelines[ VK_MAX_PIPELINES ] (vk_pipeline_slot_t).

    All pipelines share one VkPipelineLayout (vk.pipeline_layout) built by
    vk_descriptor.c:
        set 0 = global bindless descriptor set (textures + samplers)
        push constants = one range covering all shader stages, up to RHI_MAX_PUSH_CONST_SIZE

    Dynamic state:
        VK_DYNAMIC_STATE_VIEWPORT   -- set per-frame via vk_cmd_set_viewport
        VK_DYNAMIC_STATE_SCISSOR    -- set per-frame via vk_cmd_set_scissor

    Dynamic rendering (VK 1.3):
        VkPipelineRenderingCreateInfo chained into VkGraphicsPipelineCreateInfo.
        No VkRenderPass required.

==============================================================================================*/

/*==============================================================================================
    Enum conversion helpers
==============================================================================================*/

static VkCullModeFlags
rhi_cull_to_vk( rhi_cull_mode_t cull )
{
    switch ( cull )
    {
        case RHI_CULL_FRONT: return VK_CULL_MODE_FRONT_BIT;
        case RHI_CULL_BACK:  return VK_CULL_MODE_BACK_BIT;
        default:             return VK_CULL_MODE_NONE;
    }
}

static VkCompareOp
rhi_compare_to_vk( rhi_compare_op_t op )
{
    switch ( op )
    {
        case RHI_COMPARE_NEVER:         return VK_COMPARE_OP_NEVER;
        case RHI_COMPARE_LESS:          return VK_COMPARE_OP_LESS;
        case RHI_COMPARE_EQUAL:         return VK_COMPARE_OP_EQUAL;
        case RHI_COMPARE_LESS_EQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case RHI_COMPARE_GREATER:       return VK_COMPARE_OP_GREATER;
        case RHI_COMPARE_NOT_EQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
        case RHI_COMPARE_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:                        return VK_COMPARE_OP_ALWAYS;
    }
}

static VkBlendFactor
rhi_blend_factor_to_vk( rhi_blend_factor_t f )
{
    switch ( f )
    {
        case RHI_BLEND_ZERO:            return VK_BLEND_FACTOR_ZERO;
        case RHI_BLEND_ONE:             return VK_BLEND_FACTOR_ONE;
        case RHI_BLEND_SRC_COLOR:       return VK_BLEND_FACTOR_SRC_COLOR;
        case RHI_BLEND_ONE_MINUS_SRC_C: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case RHI_BLEND_SRC_ALPHA:       return VK_BLEND_FACTOR_SRC_ALPHA;
        case RHI_BLEND_ONE_MINUS_SRC_A: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case RHI_BLEND_DST_ALPHA:       return VK_BLEND_FACTOR_DST_ALPHA;
        case RHI_BLEND_ONE_MINUS_DST_A: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:                        return VK_BLEND_FACTOR_ZERO;
    }
}

static VkBlendOp
rhi_blend_op_to_vk( rhi_blend_op_t op )
{
    switch ( op )
    {
        case RHI_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case RHI_BLEND_OP_MIN:      return VK_BLEND_OP_MIN;
        case RHI_BLEND_OP_MAX:      return VK_BLEND_OP_MAX;
        default:                    return VK_BLEND_OP_ADD;
    }
}

static VkFormat
rhi_vertex_format_to_vk( rhi_vertex_format_t f )
{
    switch ( f )
    {
        case RHI_VERTEX_FORMAT_FLOAT:  return VK_FORMAT_R32_SFLOAT;
        case RHI_VERTEX_FORMAT_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
        case RHI_VERTEX_FORMAT_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
        case RHI_VERTEX_FORMAT_FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case RHI_VERTEX_FORMAT_UINT:   return VK_FORMAT_R32_UINT;
        case RHI_VERTEX_FORMAT_UINT2:  return VK_FORMAT_R32G32_UINT;
        case RHI_VERTEX_FORMAT_UINT4:  return VK_FORMAT_R32G32B32A32_UINT;
        case RHI_VERTEX_FORMAT_UNORM4: return VK_FORMAT_R8G8B8A8_UNORM;
        default:                       return VK_FORMAT_UNDEFINED;
    }
}

/*==============================================================================================
    Pipeline cache disk I/O
==============================================================================================*/

static const char* const PIPELINE_CACHE_PATH = "bin/vk_pipeline_cache.bin";

static void
vk_pipeline_cache_load( void )
{
    void*  data      = NULL;
    size_t data_size = 0;

    FILE* f = fopen( PIPELINE_CACHE_PATH, "rb" );
    if ( f )
    {
        fseek( f, 0, SEEK_END );
        long sz = ftell( f );
        fseek( f, 0, SEEK_SET );

        if ( sz > 0 )
        {
            data = malloc( (size_t)sz );
            if ( data )
            {
                data_size = fread( data, 1, (size_t)sz, f );
                if ( data_size != (size_t)sz )
                {
                    free( data );
                    data      = NULL;
                    data_size = 0;
                }
            }
        }
        fclose( f );
    }

    VkPipelineCacheCreateInfo ci = { 0 };
    ci.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data_size;
    ci.pInitialData    = data;

    VkResult r = vkCreatePipelineCache( vk.device, &ci, vk.alloc_cb, &vk.pipeline_cache );
    if ( r != VK_SUCCESS )
    {
        LOG_WARN( "pipeline_cache_load: vkCreatePipelineCache: %s; continuing without cache",
                  string_VkResult( r ) );
        vk.pipeline_cache = VK_NULL_HANDLE;
    }
    else
    {
        LOG_INFO( "pipeline_cache_load: OK (%zu bytes from disk)", data_size );
    }

    free( data );
}

static void
vk_pipeline_cache_save( void )
{
    if ( vk.pipeline_cache == VK_NULL_HANDLE )
        return;

    size_t   data_size = 0;
    VkResult r         = vkGetPipelineCacheData( vk.device, vk.pipeline_cache, &data_size, NULL );
    if ( r != VK_SUCCESS || data_size == 0 )
        return;

    void* data = malloc( data_size );
    if ( !data )
        return;

    r = vkGetPipelineCacheData( vk.device, vk.pipeline_cache, &data_size, data );
    if ( r == VK_SUCCESS )
    {
        FILE* f = fopen( PIPELINE_CACHE_PATH, "wb" );
        if ( f )
        {
            fwrite( data, 1, data_size, f );
            fclose( f );
            LOG_INFO( "pipeline_cache_save: %zu bytes written", data_size );
        }
    }

    free( data );
}

/*==============================================================================================
    Slot allocation helpers
==============================================================================================*/

static i32
vk_pipeline_alloc_slot( void )
{
    for ( u32 i = 1; i < VK_MAX_PIPELINES; ++i )
    {
        if ( vk.pipelines[ i ].pipeline == VK_NULL_HANDLE )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_pipeline_validate( rhi_pipeline_t handle )
{
    return handle.id > 0 && handle.id < VK_MAX_PIPELINES
        && vk.pipelines[ handle.id ].pipeline != VK_NULL_HANDLE;
}

/*==============================================================================================
    Pipeline creation / destruction
==============================================================================================*/

static rhi_pipeline_t
vk_pipeline_create( const rhi_pipeline_desc_t* desc )
{
    if ( !desc )
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };

    if ( !vk_shader_validate( desc->vert ) || !vk_shader_validate( desc->frag ) )
    {
        LOG_ERROR( "pipeline_create: invalid shader handle(s)" );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    i32 idx = vk_pipeline_alloc_slot();
    if ( idx < 0 )
    {
        LOG_ERROR( "pipeline pool exhausted (VK_MAX_PIPELINES = %d)", VK_MAX_PIPELINES );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    vk_pipeline_slot_t* slot     = &vk.pipelines[ (u32)idx ];
    vk_shader_slot_t*   vert_slt = &vk.shaders[ desc->vert.id ];
    vk_shader_slot_t*   frag_slt = &vk.shaders[ desc->frag.id ];

    /* --- Shader stages --- */

    VkPipelineShaderStageCreateInfo stages[ 2 ] = { 0 };
    stages[ 0 ].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[ 0 ].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[ 0 ].module = vert_slt->module;
    stages[ 0 ].pName  = vert_slt->entry;
    stages[ 1 ].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[ 1 ].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[ 1 ].module = frag_slt->module;
    stages[ 1 ].pName  = frag_slt->entry;

    /* --- Vertex input: single interleaved binding at slot 0 --- */

    VkVertexInputBindingDescription vtx_binding = { 0 };
    vtx_binding.binding   = 0;
    vtx_binding.stride    = desc->vertex_stride;
    vtx_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vtx_attribs[ RHI_MAX_VERTEX_ATTRIBS ] = { 0 };
    for ( u32 i = 0; i < desc->attrib_count; ++i )
    {
        /* Only binding 0 is declared above; attributes referencing other bindings
           would produce an invalid pipeline. Multi-binding support is not implemented. */
        ORB_ASSERT( desc->attribs[ i ].binding == 0 );
        vtx_attribs[ i ].binding  = 0;
        vtx_attribs[ i ].location = desc->attribs[ i ].location;
        vtx_attribs[ i ].offset   = desc->attribs[ i ].offset;
        vtx_attribs[ i ].format   = rhi_vertex_format_to_vk( desc->attribs[ i ].format );
    }

    VkPipelineVertexInputStateCreateInfo vtx_input     = { 0 };
    vtx_input.sType                                    = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vtx_input.vertexBindingDescriptionCount            = desc->vertex_stride > 0 ? 1 : 0;
    vtx_input.pVertexBindingDescriptions               = &vtx_binding;
    vtx_input.vertexAttributeDescriptionCount          = desc->attrib_count;
    vtx_input.pVertexAttributeDescriptions             = vtx_attribs;

    /* --- Input assembly --- */

    VkPipelineInputAssemblyStateCreateInfo input_asm = { 0 };
    input_asm.sType                                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology                               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_asm.primitiveRestartEnable                 = VK_FALSE;

    /* --- Viewport (dynamic; count required even with null ptr) --- */

    VkPipelineViewportStateCreateInfo vp_ci = { 0 };
    vp_ci.sType                             = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount                     = 1;
    vp_ci.scissorCount                      = 1;

    /* --- Rasterizer --- */

    VkPipelineRasterizationStateCreateInfo rast_ci = { 0 };
    rast_ci.sType                                  = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast_ci.polygonMode                            = VK_POLYGON_MODE_FILL;
    rast_ci.cullMode                               = rhi_cull_to_vk( desc->cull );
    rast_ci.frontFace                              = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast_ci.lineWidth                              = 1.0f;

    /* --- Multisample (no MSAA) --- */

    VkPipelineMultisampleStateCreateInfo ms_ci = { 0 };
    ms_ci.sType                                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples                 = VK_SAMPLE_COUNT_1_BIT;
    ms_ci.minSampleShading                     = 1.0f;

    /* --- Depth / stencil --- */

    VkPipelineDepthStencilStateCreateInfo depth_ci = { 0 };
    depth_ci.sType                                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_ci.depthTestEnable                       = desc->depth_test  ? VK_TRUE : VK_FALSE;
    depth_ci.depthWriteEnable                      = desc->depth_write ? VK_TRUE : VK_FALSE;
    depth_ci.depthCompareOp                        = rhi_compare_to_vk( desc->depth_compare );
    depth_ci.depthBoundsTestEnable                 = VK_FALSE;
    depth_ci.stencilTestEnable                     = VK_FALSE;

    /* --- Color blend attachments --- */

    VkPipelineColorBlendAttachmentState blend_atts[ RHI_MAX_COLOR_TARGETS ] = { 0 };
    for ( u32 i = 0; i < desc->color_target_count; ++i )
    {
        const rhi_color_target_t* ct    = &desc->color_targets[ i ];
        blend_atts[ i ].blendEnable     = ct->blend_enable ? VK_TRUE : VK_FALSE;
        blend_atts[ i ].srcColorBlendFactor = rhi_blend_factor_to_vk( ct->src_color );
        blend_atts[ i ].dstColorBlendFactor = rhi_blend_factor_to_vk( ct->dst_color );
        blend_atts[ i ].colorBlendOp    = rhi_blend_op_to_vk( ct->color_op );
        blend_atts[ i ].srcAlphaBlendFactor = rhi_blend_factor_to_vk( ct->src_alpha );
        blend_atts[ i ].dstAlphaBlendFactor = rhi_blend_factor_to_vk( ct->dst_alpha );
        blend_atts[ i ].alphaBlendOp    = rhi_blend_op_to_vk( ct->alpha_op );
        blend_atts[ i ].colorWriteMask  = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo blend_ci = { 0 };
    blend_ci.sType                               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_ci.logicOpEnable                       = VK_FALSE;
    blend_ci.attachmentCount                     = desc->color_target_count;
    blend_ci.pAttachments                        = blend_atts;

    /* --- Dynamic state --- */

    VkDynamicState dyn_states[ 2 ] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dyn_ci = { 0 };
    dyn_ci.sType                            = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount                = 2;
    dyn_ci.pDynamicStates                   = dyn_states;

    /* --- Dynamic rendering attachment info (replaces VkRenderPass) --- */

    VkFormat color_fmts[ RHI_MAX_COLOR_TARGETS ] = { 0 };
    for ( u32 i = 0; i < desc->color_target_count; ++i )
        color_fmts[ i ] = rhi_format_to_vk( desc->color_targets[ i ].format );

    VkPipelineRenderingCreateInfo rendering_ci    = { 0 };
    rendering_ci.sType                            = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount             = desc->color_target_count;
    rendering_ci.pColorAttachmentFormats          = color_fmts;
    rendering_ci.depthAttachmentFormat            = ( desc->depth_format != RHI_FORMAT_UNKNOWN )
                                                  ? rhi_format_to_vk( desc->depth_format )
                                                  : VK_FORMAT_UNDEFINED;

    /* --- Graphics pipeline --- */

    VkGraphicsPipelineCreateInfo ci = { 0 };
    ci.sType                        = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext                        = &rendering_ci;
    ci.stageCount                   = 2;
    ci.pStages                      = stages;
    ci.pVertexInputState            = &vtx_input;
    ci.pInputAssemblyState          = &input_asm;
    ci.pViewportState               = &vp_ci;
    ci.pRasterizationState          = &rast_ci;
    ci.pMultisampleState            = &ms_ci;
    ci.pDepthStencilState           = &depth_ci;
    ci.pColorBlendState             = &blend_ci;
    ci.pDynamicState                = &dyn_ci;
    ci.layout                       = vk.pipeline_layout;

    VkResult r = vkCreateGraphicsPipelines( vk.device, vk.pipeline_cache, 1, &ci,
                                            vk.alloc_cb, &slot->pipeline );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "pipeline_create: vkCreateGraphicsPipelines: %s", string_VkResult( r ) );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    if ( desc->debug_name )
        vk_debug_name_object( VK_OBJECT_TYPE_PIPELINE, (u64)slot->pipeline, desc->debug_name );

    return ( rhi_pipeline_t ){ (u32)idx };
}

static void
vk_pipeline_destroy( rhi_pipeline_t handle )
{
    if ( !vk_pipeline_validate( handle ) )
        return;

    vk_pipeline_slot_t* slot = &vk.pipelines[ handle.id ];

    vkDestroyPipeline( vk.device, slot->pipeline, vk.alloc_cb );
    slot->pipeline = VK_NULL_HANDLE;
}

/*============================================================================================*/
