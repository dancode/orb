/*==============================================================================================

    vk_pipeline_compute.c -- Compute PSO creation.

    Compute pipelines share the same slot pool (vk.pipelines[]) and pipeline layout
    (vk.pipeline_layout) as graphics pipelines.  Slot management and destruction
    live in vk_pipeline_graphics.c; this file only handles creation.

    A compute pipeline has no vertex input, rasterizer, or color blend state -- it is
    just a single compute shader stage bound to the shared pipeline layout.

==============================================================================================*/

static rhi_pipeline_t
vk_compute_pipeline_create( const rhi_compute_pipeline_desc_t* desc )
{
    if ( !desc )
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };

    if ( !vk_shader_validate( desc->comp ) )
    {
        LOG_ERROR( "compute_pipeline_create: invalid shader handle" );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    i32 idx = vk_pipeline_alloc_slot();
    if ( idx < 0 )
    {
        LOG_ERROR( "pipeline pool exhausted (VK_MAX_PIPELINES = %d)", VK_MAX_PIPELINES );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    vk_pipeline_slot_t* slot     = &vk.pipelines[ (u32)idx ];
    vk_shader_slot_t*   comp_slt = &vk.shaders[ desc->comp.id ];

    VkComputePipelineCreateInfo ci = { 0 };
    ci.sType               = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType         = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage         = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module        = comp_slt->module;
    ci.stage.pName         = comp_slt->entry;
    ci.layout              = vk.pipeline_layout;

    VkResult r = vkCreateComputePipelines( vk.device, vk.pipeline_cache, 1, &ci,
                                           vk.alloc_cb, &slot->pipeline );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "compute_pipeline_create: vkCreateComputePipelines: %s", string_VkResult( r ) );
        return ( rhi_pipeline_t ){ RHI_NULL_HANDLE };
    }

    slot->is_compute = true;

    if ( desc->debug_name )
        vk_debug_name_object( VK_OBJECT_TYPE_PIPELINE, (u64)slot->pipeline, desc->debug_name );

    return ( rhi_pipeline_t ){ (u32)idx };
}

/*============================================================================================*/
