/*==============================================================================================

    runtime_service/imgui/imgui_render.c -- GPU resource management and draw-list flush.

    imgui_render_init  creates the shared pipeline + font sampler; viewport_create makes a surface's VB/IB.
    imgui_render_flush opens a LOAD render pass on the viewport's target (preserves scene content),
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
    Per-frame geometry regions

    The CPU records up to RHI_MAX_FRAMES_IN_FLIGHT frames ahead of the GPU, so the VB/IB
    are sized to hold one independent region per in-flight slot.  Each frame writes and
    binds only its own region (selected by cmd_frame_index), so this frame's upload never
    overwrites geometry the GPU is still reading for a previous in-flight frame.
----------------------------------------------------------------------------------------------*/

#define IMGUI_VB_REGION_BYTES  ( IMGUI_MAX_VERTS * sizeof( imgui_draw_vert_t ) )
#define IMGUI_IB_REGION_BYTES  ( IMGUI_MAX_IDX   * sizeof( u16 ) )

/*----------------------------------------------------------------------------------------------
    Static GPU state : per-viewport surfaces (geometry + target) and the shared resources.

    All are created in imgui_render_init() and destroyed in imgui_render_shutdown().
----------------------------------------------------------------------------------------------*/

/* ---- Per-viewport surface (imgui_viewport_t) ----
   A viewport is a render TARGET that windows are dispatched to, not an owner of windows.  The one
   context emits every window; flush routes each window's geometry to the viewport hosting it.  So a
   viewport is a pure surface -- its own GPU geometry buffers and the color target it paints into --
   and it never assumes a single window.  There is one today (the application swapchain); a torn-off
   window will get its own, and docking will later let several windows share one.  Per-viewport so a
   surface's geometry persists between its own presents, independent of any other viewport's cadence. */

struct imgui_dock_node_t;   /* forward: the dock tree, populated when docking is implemented */

typedef struct
{
    rhi_buffer_t  vb;                   // CPU_TO_GPU vertex buffer, one region per frame-in-flight
    rhi_buffer_t  ib;                   // CPU_TO_GPU index buffer (u16), one region per frame-in-flight

    /* Color target flush paints into: RHI_SWAPCHAIN_COLOR for the main viewport, a floater's own
       swapchain image otherwise.  Held per viewport so flush is target-agnostic. */
    rhi_texture_t target;

    /* OS window this surface is hosted by (app win_id_t), or -1 (APP_WIN_INVALID) if unassociated.
       Input routing maps a mouse event's win_id to this surface so the cursor's host viewport is
       known -- a window only hover-tests when the cursor is in the OS window hosting its viewport. */
    i32 win_id;

    /* Drawable size of this surface in pixels.  Set by the host (viewport 0 from new_frame, floaters
       via viewport_resize) BEFORE the build so begin_window clips its windows against THIS surface's
       extent, not the main window's.  0 = unset -> begin_window falls back to the main display size
       (single-window behavior).  Distinct from the win_w/win_h passed to flush, which only sets the
       GPU viewport/scissor clamp at submit time; the clip baked into each draw command is built here. */
    i32 disp_w, disp_h;

    /* Docking seam.  NULL = free-float placement (today's behavior, including the main viewport's
       overlapping windows); non-NULL = a dock tree tiling/tabbing the windows on this surface.
       Inert until docking lands -- a documented placement hook, no machinery yet. */
    struct imgui_dock_node_t* dock_root;

} imgui_viewport_t;

/* Max viewports a context drives at once: the main swapchain + torn-off floaters. */
#define IMGUI_MAX_VIEWPORTS 8

/* The viewport list lives in the bound context (imgui_context_t, imgui_ctx.c): viewports[0] is the
   main swapchain, created at init.  render.c only ever touches a viewport through a passed pointer
   (viewport_create / viewport_destroy / imgui_render_flush), so it needs no instance of its own. */

/* Shared GPU resources: created once in imgui_render_init(), destroyed in imgui_render_shutdown().
   Immutable across frames and shared by every viewport (and the debug overlay), so never a per-
   viewport or per-frame bottleneck. */

static struct
{
    rhi_pipeline_t  pipeline;           // compiled pipeline with imgui shaders + vertex layout + alpha blend
    rhi_sampler_t   font_sampler;       // sampler for font textures (point clamp)
    u32             font_sampler_idx;   // bindless slot for font_sampler

    u32             draw_call_hwm;      // peak indexed draw calls in a single frame (global stat)

} s_render;

/* Manual debug toggle: flip to true (debugger, or at startup) to print the per-frame
   draw-call count every flush.  The high-water mark is always tracked and reported at
   shutdown regardless of this flag. */
static bool s_render_debug_draw_calls = false;

/* Manual debug toggle: flip to true to print the per-frame emitted geometry (vertex /
   index counts) every flush -- a direct read on render density, so the effect of UI state
   changes (e.g. collapsing a window) is visible live in the console.  Peaks are tracked in
   s_draw regardless of this flag and reported at shutdown. */
static bool s_render_debug_geometry = false;

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

/*----------------------------------------------------------------------------------------------
    viewport_create / viewport_destroy -- a surface's own GPU geometry buffers.

    Per viewport so each surface has an independent vb/ib ring (one region per frame-in-flight).
    Called once per viewport: the main swapchain at init, a torn-off floater on tear-off.  The
    shared pipeline / sampler / atlas are NOT here -- those are created once in imgui_render_init.
----------------------------------------------------------------------------------------------*/

static bool
viewport_create( imgui_viewport_t* vp, rhi_texture_t target, i32 win_id )
{
    vp->target    = target;
    vp->win_id    = win_id;     /* OS window hosting this surface; -1 = unassociated */
    vp->disp_w    = 0;          /* drawable size set by the host before build; 0 = fall back to main */
    vp->disp_h    = 0;
    vp->dock_root = NULL;       /* free-float until docking assigns a tree */

    /* Vertex buffer (CPU_TO_GPU): one region per frame-in-flight, written every frame. */
    vp->vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_VB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_vb",
    } );
    if ( !rhi_handle_valid( vp->vb ) )
        return false;

    /* Index buffer (CPU_TO_GPU, u16 indices): one region per frame-in-flight. */
    vp->ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_IB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_INDEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_ib",
    } );
    if ( !rhi_handle_valid( vp->ib ) )
    {
        rhi()->buffer_destroy( vp->vb );
        return false;
    }

    return true;
}

static void
viewport_destroy( imgui_viewport_t* vp )
{
    if ( rhi_handle_valid( vp->ib ) ) rhi()->buffer_destroy( vp->ib );
    if ( rhi_handle_valid( vp->vb ) ) rhi()->buffer_destroy( vp->vb );
    vp->vb     = ( rhi_buffer_t ){ 0 };
    vp->ib     = ( rhi_buffer_t ){ 0 };
    vp->win_id = -1;            /* slot freed -> no window matches it for input routing */
}

/*----------------------------------------------------------------------------------------------
    imgui_render_init -- create the shared GPU resources (pipeline, font sampler, atlas).
    Per-viewport geometry buffers are created separately by viewport_create.
----------------------------------------------------------------------------------------------*/

static bool
imgui_render_init( void )
{
    /* Compile shaders from embedded SPIR-V. */
    rhi_shader_t vert = rhi()->shader_load_memory(
        s_imgui_vert_spirv, sizeof( s_imgui_vert_spirv ),
        RHI_SHADER_STAGE_VERTEX, "main", "imgui_vert" );
    if ( !rhi_handle_valid( vert ) )
        return false;

    rhi_shader_t frag = rhi()->shader_load_memory(
        s_imgui_frag_spirv, sizeof( s_imgui_frag_spirv ),
        RHI_SHADER_STAGE_FRAGMENT, "main", "imgui_frag" );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
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
        return false;

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
        return false;
    }
    s_render.font_sampler_idx = rhi()->register_sampler( s_render.font_sampler );

    /* Font atlas texture -- handled by imgui_font.c.  Each atlas carries an opaque
       white texel (appended row) that solid-color draws sample, so no separate white
       texture is needed -- solids and text share the atlas and merge into one draw. */
    if ( !font_init() )
    {
        rhi()->unregister_sampler( s_render.font_sampler_idx );
        rhi()->sampler_destroy( s_render.font_sampler );
        rhi()->pipeline_destroy( s_render.pipeline );
        return false;
    }

    return true;
}

/*----------------------------------------------------------------------------------------------
    imgui_render_memory -- report GPU resource memory held by imgui (bytes).

    Buffers are sized at init and fixed; the font atlas total reflects the currently
    initialized atlases (see font_atlas_bytes).  The 1x1 white pixel is RGBA8 (4 bytes).
----------------------------------------------------------------------------------------------*/

static imgui_mem_stats_t
imgui_render_memory( void )
{
    imgui_mem_stats_t s;
    s.vertex_bytes  = RHI_MAX_FRAMES_IN_FLIGHT * (u32)IMGUI_VB_REGION_BYTES;
    s.index_bytes   = RHI_MAX_FRAMES_IN_FLIGHT * (u32)IMGUI_IB_REGION_BYTES;
    s.texture_bytes = font_atlas_bytes();   /* atlases include their appended white row */
    s.total_bytes   = s.vertex_bytes + s.index_bytes + s.texture_bytes;
    return s;
}

/*----------------------------------------------------------------------------------------------
    imgui_render_print_memory -- dump the memory breakdown to stdout (one line per bucket).
----------------------------------------------------------------------------------------------*/

static void
imgui_render_print_memory( void )
{
    imgui_mem_stats_t s = imgui_render_memory();
    const f32 kb = 1024.0f;

    printf( "[imgui] GPU memory usage:\n" );
    printf( "  vertex : %8u B  (%7.1f KB)\n", s.vertex_bytes,  s.vertex_bytes  / kb );
    printf( "  index  : %8u B  (%7.1f KB)\n", s.index_bytes,   s.index_bytes   / kb );
    printf( "  texture: %8u B  (%7.1f KB)\n", s.texture_bytes, s.texture_bytes / kb );
    printf( "  total  : %8u B  (%7.1f KB)\n", s.total_bytes,   s.total_bytes   / kb );
}

/*----------------------------------------------------------------------------------------------
    imgui_render_shutdown -- destroy all GPU resources.  Call before rhi()->shutdown().
----------------------------------------------------------------------------------------------*/

static void
imgui_render_shutdown( void )
{
    /* Peak draw-list usage over the run, so the caps can be tuned with real numbers. */
    printf( "[imgui] peak draw-list usage: verts %u/%u (%.1f%%), idx %u/%u (%.1f%%)%s\n",
            s_draw.vert_hwm, IMGUI_MAX_VERTS, 100.0f * s_draw.vert_hwm / (f32)IMGUI_MAX_VERTS,
            s_draw.idx_hwm,  IMGUI_MAX_IDX,   100.0f * s_draw.idx_hwm  / (f32)IMGUI_MAX_IDX,
            s_draw.overflow_ever ? "  -- OVERFLOWED (geometry was dropped)" : "" );

    /* Peak draw calls in a single frame -- a measure of batching effectiveness. */
    printf( "[imgui] peak draw calls in a frame: %u\n", s_render.draw_call_hwm );

    font_shutdown();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );

    if ( rhi_handle_valid( s_render.pipeline ) )
        rhi()->pipeline_destroy( s_render.pipeline );

    /* Per-viewport geometry buffers are released by viewport_destroy (driven from imgui_shutdown). */

    memset( &s_render, 0, sizeof( s_render ) );
}

/*----------------------------------------------------------------------------------------------
    imgui_render_flush -- upload draw list to GPU and emit one draw call per command.

    Paints into `vp` (the surface at index `vp_index`): uploads s_draw into vp's own vb/ib region
    and opens a LOAD pass on vp->target, so a surface's geometry and target travel together.  The
    host calls this once per live surface, each with the matching context cmd and that surface's
    drawable size; the partition is by `vp_index` -- a draw command tagged for another viewport is
    skipped here (its index range is stepped over so the kept commands keep the right offset).

    The whole vertex/index list is uploaded to every surface's buffer (geometry is shared and
    indices are absolute), and each surface draws only its own command ranges.  For the small
    per-frame geometry a debug UI emits this is cheaper than re-packing a per-viewport buffer; a
    later pass can compact it if a surface's share ever dominates.

    Opens a LOAD render pass so the scene rendered before this call is preserved.
    Sets full-window viewport; each draw command applies its own scissor rectangle.
----------------------------------------------------------------------------------------------*/

static void
imgui_render_flush( imgui_viewport_t* vp, u32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( s_draw.cmd_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    /* Track peak usage for the shutdown report.  Warn once on the first overflow:
       the draw-list push sites already drop geometry past the cap, so this just makes
       that visible.  Persistent overflow is summarized at shutdown rather than spammed. */
    if ( s_draw.vert_count > s_draw.vert_hwm ) s_draw.vert_hwm = s_draw.vert_count;
    if ( s_draw.idx_count  > s_draw.idx_hwm  ) s_draw.idx_hwm  = s_draw.idx_count;

    if ( s_draw.overflow && !s_draw.overflow_ever )
    {
        printf( "[imgui] WARNING: draw list overflow -- geometry dropped this frame "
                "(verts capped at %u, idx capped at %u). Raise IMGUI_MAX_VERTS / IMGUI_MAX_IDX.\n",
                IMGUI_MAX_VERTS, IMGUI_MAX_IDX );
    }
    if ( s_draw.overflow )
         s_draw.overflow_ever = true;

    /* Select this frame's geometry region so the upload cannot clobber data the GPU
       is still reading for another in-flight frame. */
    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = frame * (u32)IMGUI_VB_REGION_BYTES;
    u32 ib_off = frame * (u32)IMGUI_IB_REGION_BYTES;

    /* Clamp to the region capacity before writing.  The push sites already enforce this,
       so the clamp is defensive -- it guarantees the upload never exceeds the per-frame
       region even if that invariant is ever broken upstream. */
    u32 vert_count = s_draw.vert_count < IMGUI_MAX_VERTS ? s_draw.vert_count : IMGUI_MAX_VERTS;
    u32 idx_count  = s_draw.idx_count  < IMGUI_MAX_IDX   ? s_draw.idx_count  : IMGUI_MAX_IDX;

    /* Upload vertex + index data into this viewport's frame region. */
    rhi()->buffer_write( vp->vb,
                         s_draw.verts,
                         vert_count * sizeof( imgui_draw_vert_t ), vb_off );
    rhi()->buffer_write( vp->ib,
                         s_draw.indices,
                         idx_count * sizeof( u16 ), ib_off );

    /* Open a LOAD pass on the swapchain color target (no depth needed). */
    /* This is required to preserve the scene content rendered before this call; otherwise the entire
       swapchain image would be cleared and repainted with the UI every frame, which is
       very bad for performance when the UI covers only a small portion of the screen. */

    rhi_color_attachment_t color_att = {
        .texture  = vp->target,
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
    /* Bind this frame's region; index values and first_index stay region-relative. */
    rhi()->cmd_bind_vertex_buffer( cmd, vp->vb, vb_off );
    rhi()->cmd_bind_index_buffer( cmd, vp->ib, ib_off, RHI_INDEX_TYPE_UINT16 );

    /* Ortho matrix: pixel [0,w]x[0,h] -> NDC [-1,+1]x[-1,+1]. */
    imgui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );

    /* Group commands into runs of one sort key, then paint runs back-to-front by z.

       Commands sharing a sort key are contiguous in the list (immediate mode emits
       one window completely before the next), so a single forward pass collects the
       runs and the absolute first_index at each run's start.  The runs are then
       stable-sorted by z -- vertices and indices never move, only the order in which
       command ranges are replayed -- so a higher-z (raised) window paints last and
       therefore on top, with equal-z runs keeping their original order. */

    typedef struct { u32 start, count, first_index, z; } draw_run_t;
    static draw_run_t runs[ IMGUI_MAX_CMDS ];   /* single-threaded; avoids a large stack frame */
    u32 run_count = 0;

    for ( u32 i = 0, first_index = 0; i < s_draw.cmd_count; )
    {
        u32 z      = s_draw.cmd_z[ i ];
        u32 start  = i;
        u32 run_fi = first_index;
        while ( i < s_draw.cmd_count && s_draw.cmd_z[ i ] == z )
            first_index += s_draw.cmds[ i++ ].elem_count;

        runs[ run_count++ ] = ( draw_run_t ){ start, i - start, run_fi, z };
    }

    /* Stable insertion sort by z (ascending = back-to-front; run_count is small). */
    for ( u32 a = 1; a < run_count; ++a )
    {
        draw_run_t key = runs[ a ];
        u32        b   = a;
        while ( b > 0 && runs[ b - 1 ].z > key.z )
        {
            runs[ b ] = runs[ b - 1 ];
            --b;
        }
        runs[ b ] = key;
    }

    push.samp_idx = s_render.font_sampler_idx;

    u32 draw_calls = 0;   /* indexed draws actually emitted this frame (one per non-empty command) */

    for ( u32 r = 0; r < run_count; ++r )
    {
        u32 first_index = runs[ r ].first_index;
        for ( u32 k = 0; k < runs[ r ].count; ++k )
        {
            const imgui_draw_cmd_t* dc = &s_draw.cmds[ runs[ r ].start + k ];
            if ( dc->elem_count == 0 )
                continue;

            /* Partition: a command bound for another surface is not drawn here, but its index
               range is still stepped over so the commands kept on this surface address the right
               slice of the (shared, absolute) index buffer. */
            if ( s_draw.cmd_vp[ runs[ r ].start + k ] != vp_index )
            {
                first_index += dc->elem_count;
                continue;
            }

            /* Scissor to the draw command's clip rect.  Floor the origin and ceil the
               far edge (rather than truncating both) so a fractional clip rect never
               rounds inward and shaves a pixel off the visible content at a border. */
            i32 sx0 = (i32)floorf( dc->clip_rect.x );
            i32 sy0 = (i32)floorf( dc->clip_rect.y );
            i32 sx1 = (i32)ceilf ( dc->clip_rect.x + dc->clip_rect.w );
            i32 sy1 = (i32)ceilf ( dc->clip_rect.y + dc->clip_rect.h );

            /* Clamp to the framebuffer: a window dragged past the left/top edge yields a
               negative origin, which Vulkan rejects (offset must be >= 0).  Clamp the near
               edge up to 0 and the far edge down to the window extent, then guard against an
               inverted rect (fully off-screen) producing a negative width/height. */
            if ( sx0 < 0 ) sx0 = 0;
            if ( sy0 < 0 ) sy0 = 0;
            if ( sx1 > win_w ) sx1 = win_w;
            if ( sy1 > win_h ) sy1 = win_h;
            if ( sx1 < sx0 ) sx1 = sx0;
            if ( sy1 < sy0 ) sy1 = sy0;

            rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){
                .x      = sx0,
                .y      = sy0,
                .width  = sx1 - sx0,
                .height = sy1 - sy0,
            } );

            /* tex_idx is resolved at push time (solids already point at the atlas white texel). */
            push.tex_idx = dc->tex_idx;
            rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

            rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
                .index_count    = dc->elem_count,
                .instance_count = 1,
                .first_index    = first_index,
                .vertex_offset  = 0,
                .first_instance = 0,
            } );
            ++draw_calls;

            first_index += dc->elem_count;
        }
    }

    rhi()->cmd_end_rendering( cmd );

    /* Track the peak draw-call count for the shutdown report; optionally print per frame.
       Only print when the count changes from last frame, so a steady UI does not spam. */
    if ( draw_calls > s_render.draw_call_hwm )
        s_render.draw_call_hwm = draw_calls;
    static u32 prev_draw_calls = ~0u;   /* sentinel: forces a print on the first frame */
    if ( s_render_debug_draw_calls && draw_calls != prev_draw_calls )
    {
        printf( "[imgui] draw calls this frame: %u (peak %u)\n", draw_calls, s_render.draw_call_hwm );
        prev_draw_calls = draw_calls;
    }

    /* Per-frame emitted geometry -- the verts/indices this frame actually pushed into the
       draw list (peaks vs. the caps shown for context).  Watch these move as UI state
       changes; printed only when the counts differ from last frame to filter the spam. */
    static u32 prev_verts = ~0u, prev_idx = ~0u;   /* sentinel: forces a print on the first frame */
    if ( s_render_debug_geometry && ( s_draw.vert_count != prev_verts || s_draw.idx_count != prev_idx ) )
    {
        printf( "[imgui] geometry this frame: verts %u/%u (peak %u), idx %u/%u (peak %u)\n",
                s_draw.vert_count, IMGUI_MAX_VERTS, s_draw.vert_hwm,
                s_draw.idx_count,  IMGUI_MAX_IDX,   s_draw.idx_hwm );
        prev_verts = s_draw.vert_count;
        prev_idx   = s_draw.idx_count;
    }
}

// clang-format on
/*============================================================================================*/
