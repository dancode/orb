/*==============================================================================================

    runtime_service/imgui/imgui_render.c -- GPU resource management and draw-list flush.

    imgui_render_init  creates the VB, IB, pipeline, font sampler, and a 1x1 white pixel texture.
    imgui_render_flush opens a LOAD render pass on the swapchain (preserves scene content),
                       writes the current frame's vertex/index data, and emits one indexed draw
                       call per draw command in s_draw.  imgui_render_shutdown destroys all GPU objects.

    Included by imgui.c after imgui_draw.c so s_draw is in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Push constant layout (72 bytes; must match imgui_shader.h GLSL source)
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32 mvp[ 16 ];   /* column-major ortho matrix   64 bytes */
    u32 tex_idx;     /* bindless texture slot         4 bytes */
    u32 samp_idx;    /* bindless sampler slot         4 bytes */

} imgui_push_t;     /* total 72 bytes -- well within RHI_MAX_PUSH_CONST_SIZE */

/*----------------------------------------------------------------------------------------------
    Static state
----------------------------------------------------------------------------------------------*/

static struct
{
    rhi_buffer_t   vb;
    rhi_buffer_t   ib;
    rhi_pipeline_t pipeline;
    rhi_sampler_t  font_sampler;
    u32            font_sampler_idx;
    rhi_texture_t  white_tex;
    u32            white_tex_idx;

} s_render;

/*----------------------------------------------------------------------------------------------
    render_ortho -- column-major pixel-space orthographic matrix.

    Maps pixel coords ([0,w] x [0,h], origin top-left) to Vulkan NDC:
        x: [0,w] -> [-1,+1]   y: [0,h] -> [-1,+1]  (top-left is -1,-1 in Vulkan NDC)
----------------------------------------------------------------------------------------------*/

static void
render_ortho( f32 out[ 16 ], f32 w, f32 h )
{
    out[  0 ] =  2.0f / w; out[  1 ] =  0.0f;     out[  2 ] = 0.0f; out[  3 ] = 0.0f;
    out[  4 ] =  0.0f;     out[  5 ] =  2.0f / h; out[  6 ] = 0.0f; out[  7 ] = 0.0f;
    out[  8 ] =  0.0f;     out[  9 ] =  0.0f;     out[ 10 ] = 1.0f; out[ 11 ] = 0.0f;
    out[ 12 ] = -1.0f;     out[ 13 ] = -1.0f;     out[ 14 ] = 0.0f; out[ 15 ] = 1.0f;
}

/*----------------------------------------------------------------------------------------------
    imgui_render_init -- allocate all GPU resources.  Call once after rhi()->init().
----------------------------------------------------------------------------------------------*/

static bool
imgui_render_init( void )
{
    /* Vertex buffer (CPU_TO_GPU, updated every frame via buffer_write). */
    s_render.vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = IMGUI_MAX_VERTS * sizeof( imgui_draw_vert_t ),
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_vb",
    } );
    if ( !rhi_handle_valid( s_render.vb ) )
        return false;

    /* Index buffer (CPU_TO_GPU, u16 indices). */
    s_render.ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = IMGUI_MAX_IDX * sizeof( u16 ),
        .usage      = RHI_BUFFER_USAGE_INDEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_ib",
    } );
    if ( !rhi_handle_valid( s_render.ib ) )
    {
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }

    /* Compile shaders from embedded SPIR-V. */
    rhi_shader_t vert = rhi()->shader_load_memory(
        s_imgui_vert_spirv, sizeof( s_imgui_vert_spirv ),
        RHI_SHADER_STAGE_VERTEX, "main", "imgui_vert" );
    if ( !rhi_handle_valid( vert ) )
    {
        rhi()->buffer_destroy( s_render.ib );
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }

    rhi_shader_t frag = rhi()->shader_load_memory(
        s_imgui_frag_spirv, sizeof( s_imgui_frag_spirv ),
        RHI_SHADER_STAGE_FRAGMENT, "main", "imgui_frag" );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
        rhi()->buffer_destroy( s_render.ib );
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }

    /* Vertex layout: float2 pos @0, float2 uv @8, UNORM4 color @16, stride=20. */
    rhi_vertex_attrib_t attribs[ 3 ] = {
        { .binding = 0, .location = 0, .offset =  0, .format = RHI_VERTEX_FORMAT_FLOAT2 },
        { .binding = 0, .location = 1, .offset =  8, .format = RHI_VERTEX_FORMAT_FLOAT2 },
        { .binding = 0, .location = 2, .offset = 16, .format = RHI_VERTEX_FORMAT_UNORM4 },
    };

    /* Alpha blend: out = src_rgb*src_a + dst_rgb*(1-src_a). */
    rhi_color_target_t color_target = {
        .format       = RHI_FORMAT_BGRA8_SRGB,
        .blend_enable = true,
        .src_color    = RHI_BLEND_SRC_ALPHA,
        .dst_color    = RHI_BLEND_ONE_MINUS_SRC_A,
        .color_op     = RHI_BLEND_OP_ADD,
        .src_alpha    = RHI_BLEND_ONE,
        .dst_alpha    = RHI_BLEND_ONE_MINUS_SRC_A,
        .alpha_op     = RHI_BLEND_OP_ADD,
    };

    s_render.pipeline = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vert,
        .frag               = frag,
        .attribs            = { attribs[ 0 ], attribs[ 1 ], attribs[ 2 ] },
        .attrib_count       = 3,
        .vertex_stride      = sizeof( imgui_draw_vert_t ),
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( imgui_push_t ),
        .debug_name         = "imgui",
    } );

    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );

    if ( !rhi_handle_valid( s_render.pipeline ) )
    {
        rhi()->buffer_destroy( s_render.ib );
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }

    /* Font sampler: nearest filter, clamp-to-edge (no bleeding between atlas glyphs). */
    s_render.font_sampler = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_NEAREST,
        .mag_filter = RHI_FILTER_NEAREST,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( !rhi_handle_valid( s_render.font_sampler ) )
    {
        rhi()->pipeline_destroy( s_render.pipeline );
        rhi()->buffer_destroy( s_render.ib );
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }
    s_render.font_sampler_idx = rhi()->register_sampler( s_render.font_sampler );

    /* Font atlas texture -- handled by imgui_font.c. */
    if ( !font_init() )
    {
        rhi()->unregister_sampler( s_render.font_sampler_idx );
        rhi()->sampler_destroy( s_render.font_sampler );
        rhi()->pipeline_destroy( s_render.pipeline );
        rhi()->buffer_destroy( s_render.ib );
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }

    /* 1x1 opaque white RGBA8 texture for solid-color draws.
       Fragment formula: out = v_color.rgba * sample.r; with s.r=1.0 the color passes through. */
    s_render.white_tex = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = 1,
        .height       = 1,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_RGBA8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = "imgui_white",
    } );
    if ( !rhi_handle_valid( s_render.white_tex ) )
    {
        font_shutdown();
        rhi()->unregister_sampler( s_render.font_sampler_idx );
        rhi()->sampler_destroy( s_render.font_sampler );
        rhi()->pipeline_destroy( s_render.pipeline );
        rhi()->buffer_destroy( s_render.ib );
        rhi()->buffer_destroy( s_render.vb );
        return false;
    }

    const u8 white[ 4 ] = { 0xFF, 0xFF, 0xFF, 0xFF };
    rhi()->upload_texture( s_render.white_tex, white, sizeof( white ), 0, 0 );
    s_render.white_tex_idx = rhi()->register_texture( s_render.white_tex );

    return true;
}

/*----------------------------------------------------------------------------------------------
    imgui_render_shutdown -- destroy all GPU resources.  Call before rhi()->shutdown().
----------------------------------------------------------------------------------------------*/

static void
imgui_render_shutdown( void )
{
    if ( s_render.white_tex_idx )
        rhi()->unregister_texture( s_render.white_tex_idx );
    if ( rhi_handle_valid( s_render.white_tex ) )
        rhi()->texture_destroy( s_render.white_tex );

    font_shutdown();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );

    if ( rhi_handle_valid( s_render.pipeline ) )
        rhi()->pipeline_destroy( s_render.pipeline );
    if ( rhi_handle_valid( s_render.ib ) )
        rhi()->buffer_destroy( s_render.ib );
    if ( rhi_handle_valid( s_render.vb ) )
        rhi()->buffer_destroy( s_render.vb );

    memset( &s_render, 0, sizeof( s_render ) );
}

/*----------------------------------------------------------------------------------------------
    imgui_render_flush -- upload draw list to GPU and emit one draw call per command.

    Opens a LOAD render pass so the scene rendered before this call is preserved.
    Sets full-window viewport; each draw command applies its own scissor rectangle.
----------------------------------------------------------------------------------------------*/

static void
imgui_render_flush( rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( s_draw.cmd_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    /* Upload vertex + index data. */
    rhi()->buffer_write( s_render.vb,
                         s_draw.verts,
                         s_draw.vert_count * sizeof( imgui_draw_vert_t ), 0 );
    rhi()->buffer_write( s_render.ib,
                         s_draw.indices,
                         s_draw.idx_count * sizeof( u16 ), 0 );

    /* Open a LOAD pass on the swapchain color target (no depth needed). */
    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_LOAD,
        .store_op = RHI_STORE_OP_STORE,
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    /* Full-window viewport (dynamic state; must be set before first draw). */
    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f,
        .width     = (f32)win_w,
        .height    = (f32)win_h,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    } );

    rhi()->cmd_bind_pipeline( cmd, s_render.pipeline );
    rhi()->cmd_bind_bindless( cmd );
    rhi()->cmd_bind_vertex_buffer( cmd, s_render.vb, 0 );
    rhi()->cmd_bind_index_buffer( cmd, s_render.ib, 0, RHI_INDEX_TYPE_UINT16 );

    /* Ortho matrix: pixel [0,w]x[0,h] -> NDC [-1,+1]x[-1,+1]. */
    imgui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );

    u32 first_index = 0;
    for ( u32 i = 0; i < s_draw.cmd_count; ++i )
    {
        const imgui_draw_cmd_t* dc = &s_draw.cmds[ i ];
        if ( dc->elem_count == 0 )
        {
            first_index += dc->elem_count;
            continue;
        }

        /* Scissor to the draw command's clip rect. */
        rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){
            .x      = (i32)dc->clip_rect.x,
            .y      = (i32)dc->clip_rect.y,
            .width  = (i32)dc->clip_rect.w,
            .height = (i32)dc->clip_rect.h,
        } );

        /* Resolve texture index: 0 means use white pixel (solid-color draw). */
        push.tex_idx  = ( dc->tex_idx != 0 ) ? dc->tex_idx : s_render.white_tex_idx;
        push.samp_idx = s_render.font_sampler_idx;
        rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

        rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
            .index_count    = dc->elem_count,
            .instance_count = 1,
            .first_index    = first_index,
            .vertex_offset  = 0,
            .first_instance = 0,
        } );

        first_index += dc->elem_count;
    }

    rhi()->cmd_end_rendering( cmd );
}

// clang-format on
/*============================================================================================*/
