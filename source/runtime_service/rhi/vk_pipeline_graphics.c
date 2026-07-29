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
    Vertex input compatibility -- attrib VkFormat vs reflected shader input VkFormat.

    Exact equality is too strict: vertex fetch converts normalized, half, and scaled integer
    formats to 32-bit float at read time, so e.g. R8G8B8A8_UNORM legally feeds a float4 input
    and R16G16_SFLOAT legally feeds a float2 -- that is what makes packing free.  What must match
    is the NUMERIC CLASS the shader sees (float vs int vs uint), and the attrib must supply at
    least as many components as the shader reads (a wider attrib is legal; missing components
    are not).  Only the formats the RHI can actually meet are classified -- rhi_vertex_format_t
    on the attrib side, SpvReflect's 32-bit scalar/vector formats on the shader side; anything
    unrecognized falls back to requiring exact equality.
==============================================================================================*/

typedef struct
{
    u32 cls;      // 0 = unknown, 1 = float, 2 = sint, 3 = uint
    u32 comps;    // component count

} vk_vtx_class_t;

static vk_vtx_class_t
vk_vertex_format_classify( u32 fmt )
{
    switch ( ( VkFormat )fmt )
    {
        case VK_FORMAT_R32_SFLOAT:          return ( vk_vtx_class_t ){ 1, 1 };
        case VK_FORMAT_R32G32_SFLOAT:       return ( vk_vtx_class_t ){ 1, 2 };
        case VK_FORMAT_R32G32B32_SFLOAT:    return ( vk_vtx_class_t ){ 1, 3 };
        case VK_FORMAT_R32G32B32A32_SFLOAT: return ( vk_vtx_class_t ){ 1, 4 };
        case VK_FORMAT_R8G8B8A8_UNORM:      return ( vk_vtx_class_t ){ 1, 4 };   // packed, fetch widens
        case VK_FORMAT_R16G16_SFLOAT:       return ( vk_vtx_class_t ){ 1, 2 };   // packed, fetch widens
        case VK_FORMAT_R16G16B16A16_SFLOAT: return ( vk_vtx_class_t ){ 1, 4 };   // packed, fetch widens
        case VK_FORMAT_R16G16_UNORM:        return ( vk_vtx_class_t ){ 1, 2 };   // packed, fetch widens
        case VK_FORMAT_R32_SINT:            return ( vk_vtx_class_t ){ 2, 1 };
        case VK_FORMAT_R32G32_SINT:         return ( vk_vtx_class_t ){ 2, 2 };
        case VK_FORMAT_R32G32B32_SINT:      return ( vk_vtx_class_t ){ 2, 3 };
        case VK_FORMAT_R32G32B32A32_SINT:   return ( vk_vtx_class_t ){ 2, 4 };
        case VK_FORMAT_R32_UINT:            return ( vk_vtx_class_t ){ 3, 1 };
        case VK_FORMAT_R32G32_UINT:         return ( vk_vtx_class_t ){ 3, 2 };
        case VK_FORMAT_R32G32B32_UINT:      return ( vk_vtx_class_t ){ 3, 3 };
        case VK_FORMAT_R32G32B32A32_UINT:   return ( vk_vtx_class_t ){ 3, 4 };
        default:                            return ( vk_vtx_class_t ){ 0, 0 };
    }
}

/*  The packed formats are all mandatory vertex-buffer support in the Vulkan spec, but mandatory
    is a promise, not an observation -- ask the device.  A format the fetch unit cannot read
    produces undefined attribute values, i.e. silently wrong geometry; failing pipeline creation
    with the offending location named is the far cheaper outcome. */

static bool
vk_vertex_format_supported( VkFormat fmt )
{
    VkFormatProperties props = { 0 };
    vkGetPhysicalDeviceFormatProperties( vk.physical_device, fmt, &props );
    return ( props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT ) != 0;
}

static bool
vk_vertex_input_compatible( u32 attrib_fmt, u32 input_fmt )
{
    if ( attrib_fmt == input_fmt )
        return true;

    vk_vtx_class_t a = vk_vertex_format_classify( attrib_fmt );
    vk_vtx_class_t s = vk_vertex_format_classify( input_fmt );
    return a.cls != 0 && a.cls == s.cls && a.comps >= s.comps;
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

    /* When the shaders were loaded from cooked .oshd containers their reflection sits in the
       shader slots.  An empty desc (attrib_count == 0) DERIVES the layout from the vertex
       shader: attributes in location order, tightly interleaved, stride = sum of sizes.  A
       hand-filled desc is VALIDATED instead -- every shader input must be fed by a COMPATIBLE
       attribute (same numeric class and enough components; exact VkFormat equality is too
       strict because vertex fetch legally converts, e.g. a UNORM4 attrib feeding a float4
       input).  Raw-SPIR-V shaders have no reflection and the desc is trusted unchecked,
       as before. */

    const vk_shader_reflect_t* vr = vert_slt->reflect.has_data ? &vert_slt->reflect : NULL;
    const vk_shader_reflect_t* fr = frag_slt->reflect.has_data ? &frag_slt->reflect : NULL;

    /* vertex_stride == 0 suppresses the binding for bufferless draws
       (e.g., fullscreen passes that generate positions from gl_VertexIndex). */

    u32 attrib_count  = desc->attrib_count;
    u32 vertex_stride = desc->vertex_stride;

    VkVertexInputAttributeDescription vtx_attribs[ RHI_MAX_VERTEX_ATTRIBS ] = { 0 };

    if ( attrib_count == 0 && vr && vr->input_count > 0 )
    {
        u32 offset = 0;
        for ( u32 i = 0; i < vr->input_count; ++i )
        {
            vtx_attribs[ i ].binding  = 0;
            vtx_attribs[ i ].location = vr->input_location[ i ];
            vtx_attribs[ i ].offset   = offset;
            vtx_attribs[ i ].format   = ( VkFormat )vr->input_format[ i ];
            offset += vr->input_size[ i ];
        }
        attrib_count = vr->input_count;
        if ( vertex_stride == 0 )
            vertex_stride = offset;
        LOG_INFO( "pipeline_create: '%s' derived %u vertex attribs, stride %u from reflection",
                  desc->debug_name ? desc->debug_name : "(unnamed)", attrib_count, vertex_stride );
    }
    else
    {
        for ( u32 i = 0; i < attrib_count; ++i )
        {
            /* Only binding 0 is declared; multi-binding is not implemented. */
            ORB_ASSERT( desc->attribs[ i ].binding == 0 );
            vtx_attribs[ i ].binding  = 0;
            vtx_attribs[ i ].location = desc->attribs[ i ].location;
            vtx_attribs[ i ].offset   = desc->attribs[ i ].offset;
            vtx_attribs[ i ].format   = rhi_vertex_format_to_vk( desc->attribs[ i ].format );

            if ( !vk_vertex_format_supported( vtx_attribs[ i ].format ) )
            {
                LOG_ERROR( "pipeline_create: '%s' attrib at location %u uses rhi format %u "
                           "(vkfmt %u), which this device cannot fetch from a vertex buffer",
                           desc->debug_name ? desc->debug_name : "(unnamed)",
                           desc->attribs[ i ].location, ( u32 )desc->attribs[ i ].format,
                           ( u32 )vtx_attribs[ i ].format );
                return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
            }
        }

        /* Every reflected shader input must be fed; extra attributes are legal and ignored. */
        for ( u32 i = 0; vr && i < vr->input_count; ++i )
        {
            bool fed = false;
            for ( u32 j = 0; j < attrib_count && !fed; ++j )
            {
                if ( vtx_attribs[ j ].location != vr->input_location[ i ] )
                    continue;
                if ( !vk_vertex_input_compatible( ( u32 )vtx_attribs[ j ].format,
                                                  vr->input_format[ i ] ) )
                {
                    LOG_ERROR( "pipeline_create: '%s' attrib at location %u is vkfmt %u, "
                               "incompatible with the shader's vkfmt %u",
                               desc->debug_name ? desc->debug_name : "(unnamed)",
                               vr->input_location[ i ], ( u32 )vtx_attribs[ j ].format,
                               vr->input_format[ i ] );
                    return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
                }
                fed = true;
            }
            if ( !fed )
            {
                LOG_ERROR( "pipeline_create: '%s' shader input at location %u has no vertex "
                           "attribute feeding it",
                           desc->debug_name ? desc->debug_name : "(unnamed)",
                           vr->input_location[ i ] );
                return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
            }
        }
    }

    /* Push constants: the shared pipeline layout always spans RHI_MAX_PUSH_CONST_SIZE, so
       desc->push_const_size is a caller contract, not GPU state -- but reflection knows the
       real requirement, so a hand-declared size smaller than what a shader reads is refused. */

    u32 pc_need = 0;
    if ( vr && vr->pc_size > pc_need ) pc_need = vr->pc_size;
    if ( fr && fr->pc_size > pc_need ) pc_need = fr->pc_size;

    if ( ( vr || fr ) && desc->push_const_size > 0 && desc->push_const_size < pc_need )
    {
        LOG_ERROR( "pipeline_create: '%s' declares %u push constant bytes but the shaders read "
                   "%u", desc->debug_name ? desc->debug_name : "(unnamed)",
                   desc->push_const_size, pc_need );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }
    if ( ( vr || fr ) && desc->push_const_size == 0 && pc_need > 0 )
        LOG_INFO( "pipeline_create: '%s' derived push constant size %u from reflection",
                  desc->debug_name ? desc->debug_name : "(unnamed)", pc_need );

    VkVertexInputBindingDescription vtx_binding = { 0 };
    vtx_binding.binding   = 0;
    vtx_binding.stride    = vertex_stride;
    vtx_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkPipelineVertexInputStateCreateInfo vtx_input = { 0 };
    vtx_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vtx_input.vertexBindingDescriptionCount = vertex_stride > 0 ? 1 : 0;
    vtx_input.pVertexBindingDescriptions = &vtx_binding;
    vtx_input.vertexAttributeDescriptionCount = attrib_count;
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
