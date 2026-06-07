/*==============================================================================================

    vk_cmd_graphics.c -- Graphics command recording: render pass, draw calls, state binding.

    All functions here operate on an open command buffer obtained from vk_frame_begin.
    vk_cmd_begin_rendering opens a VK 1.3 dynamic render pass; vk_cmd_end_rendering
    closes it.  Draw calls must be recorded between those calls.

    vk_cmd_bind_pipeline, vk_cmd_bind_bindless, and vk_cmd_push_constants serve both
    graphics and compute pipelines -- they are here because the command buffer state
    is shared, not because they are graphics-only.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Render pass open / close
==============================================================================================*/

static void
vk_cmd_begin_rendering( rhi_cmd_list_t             cmd,
                        const rhi_color_attachment_t*  color_atts, u32 color_count,
                        const rhi_depth_attachment_t*  depth_att )
{
    if ( !cmd )
        return;

    vk_context_t* ctx = vk_ctx_get( cmd->ctx_id );
    if ( !ctx )
        return;

    /* Build color attachment infos. */
    VkRenderingAttachmentInfo color_infos[ RHI_MAX_COLOR_TARGETS ] = { 0 };
    u32 cc = ( color_count < RHI_MAX_COLOR_TARGETS ) ? color_count : RHI_MAX_COLOR_TARGETS;
    for ( u32 i = 0; i < cc; i++ )
    {
        const rhi_color_attachment_t* ca = &color_atts[ i ];
        VkRenderingAttachmentInfo*    ai = &color_infos[ i ];

        ai->sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ai->imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ai->loadOp      = vk_load_op( ca->load_op );
        ai->storeOp     = vk_store_op( ca->store_op );
        ai->imageView   = ( ca->texture.id == RHI_SWAPCHAIN_COLOR )
                          ? ctx->swapchain_image_views[ ctx->image_index ]
                          : vk.textures[ ca->texture.id ].view;

        if ( ca->load_op == RHI_LOAD_OP_CLEAR )
        {
            ai->clearValue.color.float32[ 0 ] = ca->clear.r;
            ai->clearValue.color.float32[ 1 ] = ca->clear.g;
            ai->clearValue.color.float32[ 2 ] = ca->clear.b;
            ai->clearValue.color.float32[ 3 ] = ca->clear.a;
        }
    }

    /* Build depth attachment info. */
    VkRenderingAttachmentInfo depth_info = { 0 };
    if ( depth_att )
    {
        depth_info.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_info.loadOp      = vk_load_op( depth_att->load_op );
        depth_info.storeOp     = vk_store_op( depth_att->store_op );
        depth_info.imageView   = ( depth_att->texture.id == RHI_SWAPCHAIN_DEPTH )
                                 ? ctx->depth_view[ cmd->frame ]
                                 : vk.textures[ depth_att->texture.id ].view;

        if ( depth_att->load_op == RHI_LOAD_OP_CLEAR )
        {
            depth_info.clearValue.depthStencil.depth   = depth_att->depth_clear;
            depth_info.clearValue.depthStencil.stencil = depth_att->stencil_clear;
        }
    }

    VkRenderingInfo ri               = { 0 };
    ri.sType                         = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset             = (VkOffset2D){ 0, 0 };
    ri.renderArea.extent             = ctx->swapchain_extent;
    ri.layerCount                    = 1;
    ri.colorAttachmentCount          = cc;
    ri.pColorAttachments             = cc > 0 ? color_infos : NULL;
    ri.pDepthAttachment              = depth_att ? &depth_info : NULL;

    vkCmdBeginRendering( cmd->vk_cmd, &ri );
}

static void
vk_cmd_end_rendering( rhi_cmd_list_t cmd )
{
    if ( !cmd )
        return;

    vkCmdEndRendering( cmd->vk_cmd );
}

/*==============================================================================================
    Viewport and scissor state
==============================================================================================*/

static void
vk_cmd_set_viewport( rhi_cmd_list_t cmd, const rhi_viewport_t* vp )
{
    if ( !cmd || !vp )
        return;

    VkViewport vkvp;
    vkvp.x        = vp->x;
    vkvp.y        = vp->y;
    vkvp.width    = vp->width;
    vkvp.height   = vp->height;
    vkvp.minDepth = vp->min_depth;
    vkvp.maxDepth = vp->max_depth;
    vkCmdSetViewport( cmd->vk_cmd, 0, 1, &vkvp );
}

static void
vk_cmd_set_scissor( rhi_cmd_list_t cmd, const rhi_rect_t* rect )
{
    if ( !cmd || !rect )
        return;

    VkRect2D sc;
    sc.offset.x      = rect->x;
    sc.offset.y      = rect->y;
    sc.extent.width  = (u32)rect->width;
    sc.extent.height = (u32)rect->height;
    vkCmdSetScissor( cmd->vk_cmd, 0, 1, &sc );
}

/*==============================================================================================
    Shared pipeline / descriptor binding  (serves both graphics and compute bind points)
==============================================================================================*/

static void
vk_cmd_bind_pipeline( rhi_cmd_list_t cmd, rhi_pipeline_t pipeline )
{
    if ( !cmd || !vk_pipeline_validate( pipeline ) )
        return;

    vk_pipeline_slot_t* slot = &vk.pipelines[ pipeline.id ];
    VkPipelineBindPoint bp   = slot->is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline( cmd->vk_cmd, bp, slot->pipeline );
}

static void
vk_cmd_bind_bindless( rhi_cmd_list_t cmd )
{
    if ( !cmd )
        return;

    /* Bind for both points: descriptor set state is per-bind-point in Vulkan, and the
       pipeline layout covers compute (push constant range includes COMPUTE_BIT). */
    vkCmdBindDescriptorSets( cmd->vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             vk.pipeline_layout, 0, 1, &vk.bindless_set, 0, NULL );
    vkCmdBindDescriptorSets( cmd->vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             vk.pipeline_layout, 0, 1, &vk.bindless_set, 0, NULL );
}

static void
vk_cmd_push_constants( rhi_cmd_list_t cmd, const void* data, u32 size, u32 offset )
{
    if ( !cmd || !data || size == 0 )
        return;

    vkCmdPushConstants( cmd->vk_cmd, vk.pipeline_layout,
                        VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
                        offset, size, data );
}

/*==============================================================================================
    Vertex / index buffer binding
==============================================================================================*/

static void
vk_cmd_bind_vertex_buffer( rhi_cmd_list_t cmd, rhi_buffer_t buf, u32 offset )
{
    if ( !cmd || !vk_buffer_validate( buf ) )
        return;

    VkBuffer     vkb = vk.buffers[ buf.id ].buffer;
    VkDeviceSize off = offset;
    vkCmdBindVertexBuffers( cmd->vk_cmd, 0, 1, &vkb, &off );
}

static void
vk_cmd_bind_index_buffer( rhi_cmd_list_t cmd, rhi_buffer_t buf, u32 offset,
                           rhi_index_type_t type )
{
    if ( !cmd || !vk_buffer_validate( buf ) )
        return;

    VkBuffer    vkb  = vk.buffers[ buf.id ].buffer;
    VkIndexType vkt  = ( type == RHI_INDEX_TYPE_UINT16 ) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer( cmd->vk_cmd, vkb, offset, vkt );
}

/*==============================================================================================
    Draw calls
==============================================================================================*/

static void
vk_cmd_draw( rhi_cmd_list_t cmd, const rhi_draw_args_t* args )
{
    if ( !cmd || !args )
        return;

    vkCmdDraw( cmd->vk_cmd, args->vertex_count, args->instance_count,
               args->first_vertex, args->first_instance );
}

static void
vk_cmd_draw_indexed( rhi_cmd_list_t cmd, const rhi_draw_indexed_args_t* args )
{
    if ( !cmd || !args )
        return;

    vkCmdDrawIndexed( cmd->vk_cmd, args->index_count, args->instance_count,
                      args->first_index, args->vertex_offset, args->first_instance );
}

static void
vk_cmd_draw_indirect( rhi_cmd_list_t cmd, rhi_buffer_t buf, u32 offset, u32 draw_count, u32 stride )
{
    if ( !cmd || !vk_buffer_validate( buf ) ) return;
    vkCmdDrawIndirect( cmd->vk_cmd, vk.buffers[ buf.id ].buffer, offset, draw_count, stride );
}

static void
vk_cmd_draw_indexed_indirect( rhi_cmd_list_t cmd, rhi_buffer_t buf, u32 offset, u32 draw_count, u32 stride )
{
    if ( !cmd || !vk_buffer_validate( buf ) ) return;
    vkCmdDrawIndexedIndirect( cmd->vk_cmd, vk.buffers[ buf.id ].buffer, offset, draw_count, stride );
}

/*============================================================================================*/
// clang-format on
