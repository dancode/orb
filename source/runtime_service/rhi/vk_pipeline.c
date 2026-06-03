/*==============================================================================================

    vulkan/vk_pipeline.c -- Graphics and compute PSO creation with a persistent cache.

    Slot pool: vk.pipelines[ VK_MAX_PIPELINES ] (vk_pipeline_slot_t).

    All pipelines share one VkPipelineLayout (vk.pipeline_layout) built by
    vk_descriptor.c:
        set 0 = global bindless descriptor set (textures, samplers, storage buffers)
        push constants = one range covering all shader stages, up to RHI_MAX_PUSH_CONST_SIZE

    Dynamic state used:
        VK_DYNAMIC_STATE_VIEWPORT       (set per-frame via cmd_set_viewport)
        VK_DYNAMIC_STATE_SCISSOR        (set per-frame via cmd_set_scissor)

    Dynamic rendering (VK 1.3):
        VkPipelineRenderingCreateInfo is chained into VkGraphicsPipelineCreateInfo.
        No VkRenderPass is required.

    Pipeline cache:
        vk.pipeline_cache is initialized at device creation.
        Optionally loaded from a file on disk and serialized back at shutdown for
        faster warm-starts (driver can skip some shader compilation work).

==============================================================================================*/

/*==============================================================================================
    Pipeline cache disk I/O
==============================================================================================*/

static void
vk_pipeline_cache_load( void )
{
    /* TODO:
       Read "bin/vk_pipeline_cache.bin" if it exists.
       VkPipelineCacheCreateInfo ci = {
           .initialDataSize = file_size,
           .pInitialData    = file_data,
       };
       vkCreatePipelineCache( vk.device, &ci, vk.alloc_cb, &vk.pipeline_cache )

       If file not found: ci.initialDataSize = 0; still create the cache object so
       subsequent pipeline creates can populate it.
    */
    printf( "[rhi:vk] pipeline_cache_load (placeholder)\n" );
}

static void
vk_pipeline_cache_save( void )
{
    /* TODO:
       vkGetPipelineCacheData( vk.device, vk.pipeline_cache, &data_size, NULL )
       allocate data_size bytes, call again with the buffer, write to disk.
    */
}

/*==============================================================================================
    Slot allocation helpers
==============================================================================================*/

static i32
vk_pipeline_alloc_slot( void )
{
    for ( u32 i = 0; i < VK_MAX_PIPELINES; ++i )
    {
        if ( vk.pipelines[ i ].generation == 0 )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_pipeline_validate( rhi_pipeline_t handle )
{
    u32 idx = VK_HANDLE_IDX( handle.id );
    u8  gen = VK_HANDLE_GEN( handle.id );
    return idx < VK_MAX_PIPELINES && gen != 0 && vk.pipelines[ idx ].generation == gen;
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

    vk_pipeline_slot_t* slot = &vk.pipelines[ idx ];
    u8 gen = ( u8 )( slot->generation == 0 ? 1 : slot->generation );

    /* TODO (Vulkan implementation):

       VkShaderModule vert_mod = vk.shaders[ VK_HANDLE_IDX(desc->vert.id) ].module;
       VkShaderModule frag_mod = vk.shaders[ VK_HANDLE_IDX(desc->frag.id) ].module;

       VkPipelineShaderStageCreateInfo stages[2] = {
           { .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vert_mod, .pName = "main" },
           { .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag_mod, .pName = "main" },
       };

       Build VkVertexInputBindingDescription and VkVertexInputAttributeDescription[]
       from desc->attribs[] and desc->vertex_stride.

       VkPipelineVertexInputStateCreateInfo   vertex_input  = { ... };
       VkPipelineInputAssemblyStateCreateInfo input_assembly = {
           .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
       VkPipelineRasterizationStateCreateInfo rasterizer = {
           .cullMode  = rhi_cull_to_vk( desc->cull ),
           .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
       };
       VkPipelineDepthStencilStateCreateInfo depth_stencil = {
           .depthTestEnable  = desc->depth_test  ? VK_TRUE : VK_FALSE,
           .depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE,
           .depthCompareOp   = rhi_compare_to_vk( desc->depth_compare ),
       };

       Build VkPipelineColorBlendAttachmentState[] from desc->color_targets[].

       VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

       Dynamic rendering attachment info (no VkRenderPass):
       VkFormat color_fmts[ RHI_MAX_COLOR_TARGETS ];
       for ( u32 i = 0; i < desc->color_target_count; ++i )
           color_fmts[i] = rhi_format_to_vk( desc->color_targets[i].format );
       VkPipelineRenderingCreateInfo rendering_ci = {
           .colorAttachmentCount    = desc->color_target_count,
           .pColorAttachmentFormats = color_fmts,
           .depthAttachmentFormat   = rhi_format_to_vk( desc->depth_format ),
       };

       VkGraphicsPipelineCreateInfo ci = {
           .pNext               = &rendering_ci,
           .stageCount          = 2,
           .pStages             = stages,
           .pVertexInputState   = &vertex_input,
           .pInputAssemblyState = &input_assembly,
           .pRasterizationState = &rasterizer,
           .pDepthStencilState  = &depth_stencil,
           .pColorBlendState    = &blend,
           .pDynamicState       = &dyn_state,
           .layout              = vk.pipeline_layout,
       };
       vkCreateGraphicsPipelines( vk.device, vk.pipeline_cache, 1, &ci,
                                  vk.alloc_cb, &slot->pipeline )

       if ( desc->debug_name )
           vk_debug_name_object( VK_OBJECT_TYPE_PIPELINE, (u64)slot->pipeline, desc->debug_name )
    */

    slot->generation = gen;
    return ( rhi_pipeline_t ){ VK_MAKE_HANDLE( gen, ( u32 )idx ) };
}

static void
vk_pipeline_destroy( rhi_pipeline_t handle )
{
    if ( !vk_pipeline_validate( handle ) )
        return;

    u32                 idx  = VK_HANDLE_IDX( handle.id );
    vk_pipeline_slot_t* slot = &vk.pipelines[ idx ];

    /* TODO: vkDestroyPipeline( vk.device, slot->pipeline, vk.alloc_cb ) */

    slot->generation = ( u8 )( slot->generation + 1 );
    slot->pipeline   = VK_NULL_HANDLE;
}

/*============================================================================================*/
