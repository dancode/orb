/*==============================================================================================

    vk_pipeline_graphics.c -- Graphics PSO creation and shared pipeline slot management.

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
    Slot allocation helpers  (shared by graphics and compute)
==============================================================================================*/

static i32
vk_pipeline_alloc_slot( void )
{
    for ( i32 i = 1; i < VK_MAX_PIPELINES; ++i ) 
    {
        if ( vk.pipelines[ i ].pipeline == VK_NULL_HANDLE )
            return i;
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
    Graphics PSO creation

    The core factory for creating Graphics Pipeline State Objects (PSOs). 
    In Vulkan, a PSO is a "baked" state that includes almost everything the GPU needs to
    know to execute a draw call (shader + vertex format + raster state + blend state + etc.).

==============================================================================================*/

static rhi_pipeline_t
vk_pipeline_create( const rhi_pipeline_desc_t* desc )
{
    if ( !desc )
         return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };

    if ( !vk_shader_validate( desc->vert ) || !vk_shader_validate( desc->frag )) {
         LOG_ERROR( "invalid shader handle(s)" );
         return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    i32  pipeline_id = vk_pipeline_alloc_slot();
    if ( pipeline_id < 0 ) {
         LOG_ERROR( "pipeline pool exhausted (VK_MAX_PIPELINES = %d)", VK_MAX_PIPELINES );
         return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    vk_pipeline_slot_t* slot     = &vk.pipelines[ pipeline_id ];
    vk_shader_slot_t*   vert_slt = &vk.shaders[ desc->vert.id ];
    vk_shader_slot_t*   frag_slt = &vk.shaders[ desc->frag.id ];

    /* --- Shader stages --- */

    /* Vert + frag only; geometry, tessellation, and compute stages are not implemented. */

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

    /* vertex_stride == 0 suppresses the binding for bufferless draws
       (e.g., fullscreen passes that generate positions from gl_VertexIndex). */

    VkVertexInputBindingDescription vtx_binding = { 0 };
    vtx_binding.binding   = 0;
    vtx_binding.stride    = desc->vertex_stride;
    vtx_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vtx_attribs[ RHI_MAX_VERTEX_ATTRIBS ] = { 0 };
    for ( u32 i = 0; i < desc->attrib_count; ++i )
    {
        /* Only binding 0 is declared; multi-binding is not implemented. */
        ORB_ASSERT( desc->attribs[ i ].binding == 0 );
        vtx_attribs[ i ].binding  = 0;
        vtx_attribs[ i ].location = desc->attribs[ i ].location;
        vtx_attribs[ i ].offset   = desc->attribs[ i ].offset;
        vtx_attribs[ i ].format   = rhi_vertex_format_to_vk( desc->attribs[ i ].format );
    }

    VkPipelineVertexInputStateCreateInfo vtx_input = { 0 };
    vtx_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vtx_input.vertexBindingDescriptionCount = desc->vertex_stride > 0 ? 1 : 0;
    vtx_input.pVertexBindingDescriptions = &vtx_binding;
    vtx_input.vertexAttributeDescriptionCount = desc->attrib_count;
    vtx_input.pVertexAttributeDescriptions = vtx_attribs;

    /* --- Input assembly --- */
    /* Triangle list is the only topology in use; strips would require restart-index logic. */

    VkPipelineInputAssemblyStateCreateInfo input_asm = { 0 };
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_asm.primitiveRestartEnable = VK_FALSE;

    /* --- Viewport (dynamic; count required even with null ptr) --- */
    /* pViewport/pScissor may be null here because the dynamic state owns the values at draw time. */

    VkPipelineViewportStateCreateInfo vp_ci = { 0 };
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.scissorCount = 1;

    /* --- Rasterizer --- */
    /* CCW winding follows the right-hand projection convention; depth bias disabled. */

    VkPipelineRasterizationStateCreateInfo rast_ci = { 0 };
    rast_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast_ci.polygonMode = rhi_polygon_to_vk( desc->polygon_mode );
    rast_ci.cullMode = rhi_cull_to_vk( desc->cull );
    rast_ci.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast_ci.lineWidth = 1.0f;

    /* --- Multisample (no MSAA) --- */
    /* Single-sample only; swapchain and all render targets carry no MSAA, no resolve pass exists. */

    VkPipelineMultisampleStateCreateInfo ms_ci = { 0 };
    ms_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms_ci.minSampleShading = 1.0f;

    /* --- Depth / stencil --- */
    /* Stencil is globally disabled; no pass uses it and it avoids format 
       requirements on the attachment. */

    VkPipelineDepthStencilStateCreateInfo depth_ci = { 0 };
    depth_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_ci.depthTestEnable  = desc->depth_test  ? VK_TRUE : VK_FALSE;
    depth_ci.depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE;
    depth_ci.depthCompareOp = rhi_compare_to_vk( desc->depth_compare );
    depth_ci.depthBoundsTestEnable = VK_FALSE;
    depth_ci.stencilTestEnable = VK_FALSE;

    /* --- Color blend attachments --- */
    /* Per-attachment blend state allows MRT targets to mix independently; full RGBA write mask. */

    VkPipelineColorBlendAttachmentState blend_atts[ RHI_MAX_COLOR_TARGETS ] = { 0 };
    for ( u32 i = 0; i < desc->color_target_count; ++i )
    {
        const rhi_color_target_t* ct        = &desc->color_targets[ i ];
        blend_atts[ i ].blendEnable         = ct->blend_enable ? VK_TRUE : VK_FALSE;
        blend_atts[ i ].srcColorBlendFactor = rhi_blend_factor_to_vk( ct->src_color );
        blend_atts[ i ].dstColorBlendFactor = rhi_blend_factor_to_vk( ct->dst_color );
        blend_atts[ i ].colorBlendOp        = rhi_blend_op_to_vk( ct->color_op );
        blend_atts[ i ].srcAlphaBlendFactor = rhi_blend_factor_to_vk( ct->src_alpha );
        blend_atts[ i ].dstAlphaBlendFactor = rhi_blend_factor_to_vk( ct->dst_alpha );
        blend_atts[ i ].alphaBlendOp        = rhi_blend_op_to_vk( ct->alpha_op );
        blend_atts[ i ].colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo blend_ci = { 0 };
    blend_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_ci.logicOpEnable = VK_FALSE;
    blend_ci.attachmentCount = desc->color_target_count;
    blend_ci.pAttachments = blend_atts;

    /* --- Dynamic state --- */
    /* Minimum set: viewport + scissor keeps the PSO valid across swapchain 
       resizes without recreation. */

    VkDynamicState dyn_states[ 2 ] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dyn_ci = { 0 };
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dyn_states;

    /* --- Dynamic rendering (VK 1.3, replaces VkRenderPass) --- */
    /* Attachment formats bake into the PSO; they must match the actual render targets at draw time. */

    VkFormat color_fmts[ RHI_MAX_COLOR_TARGETS ] = { 0 };
    for ( u32 i = 0; i < desc->color_target_count; ++i )
        color_fmts[ i ] = rhi_format_to_vk( desc->color_targets[ i ].format );

    VkPipelineRenderingCreateInfo rendering_ci    = { 0 };
    rendering_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount = desc->color_target_count;
    rendering_ci.pColorAttachmentFormats = color_fmts;
    rendering_ci.depthAttachmentFormat = ( desc->depth_format != RHI_FORMAT_UNKNOWN )
                       ? rhi_format_to_vk( desc->depth_format ): VK_FORMAT_UNDEFINED;

    /* --- Graphics pipeline --- */
    /* One global layout for all shaders: bindless descriptor set + push constants.
       Eliminates layout-switch overhead when changing pipelines within a frame. */

    VkGraphicsPipelineCreateInfo ci = { 0 };
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &rendering_ci;
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vtx_input;
    ci.pInputAssemblyState = &input_asm;
    ci.pViewportState      = &vp_ci;
    ci.pRasterizationState = &rast_ci;
    ci.pMultisampleState   = &ms_ci;
    ci.pDepthStencilState  = &depth_ci;
    ci.pColorBlendState    = &blend_ci;
    ci.pDynamicState       = &dyn_ci;
    ci.layout              = vk.pipeline_layout;

    VkResult r = vkCreateGraphicsPipelines( vk.device, vk.pipeline_cache, 1, &ci,
                                            vk.alloc_cb, &slot->pipeline );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "pipeline_create: vkCreateGraphicsPipelines: %s", string_VkResult( r ) );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    if ( desc->debug_name )
        vk_debug_name_object( VK_OBJECT_TYPE_PIPELINE, (u64)slot->pipeline, desc->debug_name );

    return ( rhi_pipeline_t ){ (u32)pipeline_id };
}

/*==============================================================================================
    Pipeline destruction  (shared; destroys both graphics and compute slots)
==============================================================================================*/

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
