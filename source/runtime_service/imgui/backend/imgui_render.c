/*==============================================================================================

    runtime_service/imgui/imgui_render.c -- GPU resources + draw submission (SUBMIT phase).

    The last of the three render phases (see imgui_render_cache.c for the full map):

        EMIT   imgui_draw.c          widgets -> s_draw semantic command list
        BUILD  imgui_render_cache.c  diff + tessellate -> s_tess geometry + s_dispatch slot table
        SUBMIT this file             upload each surface's slots + emit indexed draw calls

    Two responsibilities live here:

      - GPU resources (imgui_render_init / _shutdown): the shared pipeline + font sampler, created
        once; and a surface's own vertex/index buffers (viewport_create / viewport_destroy), created
        per render target.  These are immutable across frames and shared by every surface.

      - The flush (imgui_render_flush): kick the once-per-frame BUILD (cache_build_frame, lazy), then
        upload this surface's slice of the shared geometry and emit one indexed draw call per cached
        GPU command, back-to-front in dispatch order.

    Included by imgui_backend.c after imgui_render_cache.c (cache_build_frame, s_dispatch, the slot
    types, the stats accessors) -- which in turn follows imgui_render_tess.c (s_tess) and
    imgui_draw.c (s_draw).  imgui_debug.c follows this file and reuses s_render + render_ortho.

==============================================================================================*/
#include "runtime_service/imgui/imgui_internal.h"   // imgui_viewport_t, imgui_context_t, IMGUI_MAX_VIEWPORTS
// clang-format off

/*==============================================================================================
    Push constant layout (80 bytes; must match imgui_shader.h GLSL source)
==============================================================================================*/

typedef struct
{
    f32 mvp[ 16 ];      // column-major ortho matrix       64 bytes
    u32 tex_idx;        // bindless texture slot            4 bytes
    u32 samp_idx;       // bindless sampler slot            4 bytes
    u32 dbg_flat;       // debug: 1 = flat color (no atlas) 4 bytes
    u32 dbg_tint;       // debug: packed RGBA8 batch tint   4 bytes

} imgui_push_t;         // total 80 bytes -- well within RHI_MAX_PUSH_CONST_SIZE

/*----------------------------------------------------------------------------------------------
    Per-frame geometry regions.

    The CPU records up to RHI_MAX_FRAMES_IN_FLIGHT frames ahead of the GPU, so each surface's VB/IB
    holds one independent region per in-flight slot.  Each frame writes and binds only its own region
    (selected by cmd_frame_index), so this frame's upload never overwrites geometry the GPU is still
    reading for a previous in-flight frame.
----------------------------------------------------------------------------------------------*/

#define IMGUI_VB_REGION_BYTES  ( IMGUI_MAX_VERTS * sizeof( imgui_draw_vert_t ) )
#define IMGUI_IB_REGION_BYTES  ( IMGUI_MAX_IDX   * sizeof( u16 ) )

/*----------------------------------------------------------------------------------------------
    Shared GPU resources -- created once in imgui_render_init, destroyed in imgui_render_shutdown.

    Immutable across frames and shared by every viewport (and the debug overlay), so never a
    per-viewport or per-frame bottleneck.  Per-viewport surfaces own only their vb/ib (in
    imgui_viewport_t); a viewport is a render TARGET that windows are dispatched to, not an owner of
    windows -- the one context emits every window and flush routes each window's geometry to the
    viewport hosting it.  imgui_viewport_t + IMGUI_MAX_VIEWPORTS live in imgui_internal.h; the
    viewport list itself lives in the bound context (imgui_ctx.c), so this file only ever touches a
    viewport through a passed pointer.
----------------------------------------------------------------------------------------------*/

static struct
{
    rhi_pipeline_t  pipeline;           // compiled pipeline: imgui shaders + vertex layout + alpha blend
    rhi_pipeline_t  pipeline_wire;      // same pipeline in VK_POLYGON_MODE_LINE (wireframe debug view)
    rhi_sampler_t   font_sampler;       // sampler for font textures (point clamp)
    u32             font_sampler_idx;   // bindless slot for font_sampler

    imgui_render_mode_t debug_mode;     // NORMAL / WIREFRAME / BATCH -- how the UI list is rasterized

} s_render;

/* Flush-side debug toggle: flip to true to print the per-frame draw-call count whenever it changes.
   (The peak is tracked unconditionally in the cache stats and reported at shutdown.) */
static bool s_dbg_draw_calls = false;

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

/*==============================================================================================
    Per-viewport surfaces -- a surface's own GPU geometry buffers.

    Per viewport so each surface has an independent vb/ib ring (one region per frame-in-flight).
    Called once per viewport: the main swapchain at init, a torn-off floater on tear-off.  The shared
    pipeline / sampler / atlas are NOT here -- those are created once in imgui_render_init.
==============================================================================================*/

bool
viewport_create( imgui_viewport_t* vp, rhi_texture_t target, i32 win_id )
{
    vp->target    = target;
    vp->win_id    = win_id;           // OS window hosting this surface; -1 = unassociated
    vp->rhi_ctx   = RHI_CTX_INVALID;  // set only by viewport_spawn for an imgui-owned floater
    vp->owned     = false;            // host-provided unless viewport_spawn flips it
    vp->pending_close = false;        // owned floater close request; serviced by viewport_update
    vp->disp_w    = 0;                // drawable size set by the host before build; 0 = fall back to main
    vp->disp_h    = 0;
    vp->caption_inset = 0.0f;         // no native shell band until one publishes it during the build
    vp->dock_root = NULL;             // free-float until docking assigns a tree

    // Vertex buffer (CPU_TO_GPU): one region per frame-in-flight, written every frame.
    vp->vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_VB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_vb",
    } );
    if ( !rhi_handle_valid( vp->vb ) )
        return false;

    // Index buffer (CPU_TO_GPU, u16 indices): one region per frame-in-flight.
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

void
viewport_destroy( imgui_viewport_t* vp )
{
    /* Owned floater: destroy the rhi context FIRST.  context_destroy idles the GPU (waits the device)
       before tearing down its swapchain/sync, so the geometry-buffer frees below are then safe --
       matching the host's own shutdown order (drain, free buffers, close window).  A host-provided
       surface (owned == false) leaves its context to the host; imgui frees only the GPU buffers it
       created via viewport_create. */
    if ( vp->owned && vp->rhi_ctx != RHI_CTX_INVALID )
        rhi()->context_destroy( vp->rhi_ctx );

    if ( rhi_handle_valid( vp->ib ) ) rhi()->buffer_destroy( vp->ib );
    if ( rhi_handle_valid( vp->vb ) ) rhi()->buffer_destroy( vp->vb );

    // Owned floater: close the OS window imgui opened, only after the context (and its swapchain) is gone.
    if ( vp->owned && vp->win_id >= 0 )
        app()->window_close( vp->win_id );

    vp->vb            = ( rhi_buffer_t ){ 0 };
    vp->ib            = ( rhi_buffer_t ){ 0 };
    vp->win_id        = -1;            // slot freed -> no window matches it for input routing
    vp->rhi_ctx       = RHI_CTX_INVALID;
    vp->owned         = false;
    vp->pending_close = false;
}

/*==============================================================================================
    Init / shutdown -- the shared GPU resources (pipeline, font sampler, atlas).
==============================================================================================*/

bool
imgui_render_init( void )
{
    // Compile shaders from embedded SPIR-V.
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

    // Vertex layout: float2 pos @0, float2 uv @8, UNORM4 color @16, stride=20.
    rhi_vertex_attrib_t attribs[ 3 ] = {
        { .binding = 0, .location = 0, .offset =  0, .format = RHI_VERTEX_FORMAT_FLOAT2 },
        { .binding = 0, .location = 1, .offset =  8, .format = RHI_VERTEX_FORMAT_FLOAT2 },
        { .binding = 0, .location = 2, .offset = 16, .format = RHI_VERTEX_FORMAT_UNORM4 },
    };

    // Alpha blend: out = src_rgb*src_a + dst_rgb*(1-src_a).
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

    /* One descriptor shared by both pipelines; only polygon_mode differs.  The wireframe variant
       (VK_POLYGON_MODE_LINE) lets the debug render mode draw triangle edges through the same shaders,
       vertex layout, and push range -- the flush just binds whichever the mode selects. */
    rhi_pipeline_desc_t pdesc = {
        .vert               = vert,
        .frag               = frag,
        .attribs            = { attribs[ 0 ], attribs[ 1 ], attribs[ 2 ] },
        .attrib_count       = 3,
        .vertex_stride      = sizeof( imgui_draw_vert_t ),
        .cull               = RHI_CULL_NONE,
        .polygon_mode       = RHI_POLYGON_FILL,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( imgui_push_t ),
        .debug_name         = "imgui",
    };
    s_render.pipeline = rhi()->pipeline_create( &pdesc );

    pdesc.polygon_mode = RHI_POLYGON_LINE;
    pdesc.debug_name   = "imgui_wire";
    s_render.pipeline_wire = rhi()->pipeline_create( &pdesc );

    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );

    /* The wireframe pipeline is a debug convenience -- a failure there is non-fatal (the mode just
       falls back to the fill pipeline at flush time); only the fill pipeline is required. */
    if ( !rhi_handle_valid( s_render.pipeline ) )
        return false;

    /* Font sampler: nearest filter.  V/W clamp-to-edge (no bleeding between atlas glyph rows); U
       repeats so a dashed line's single quad can tile an atlas stipple row along its length.  Glyph
       and white-texel U coords stay within [0,1], so wrapping never affects text or fills. */
    s_render.font_sampler = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_NEAREST,
        .mag_filter = RHI_FILTER_NEAREST,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_REPEAT,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( !rhi_handle_valid( s_render.font_sampler ) )
    {
        rhi()->pipeline_destroy( s_render.pipeline );
        return false;
    }
    s_render.font_sampler_idx = rhi()->register_sampler( s_render.font_sampler );

    /* Font atlas texture -- handled by imgui_font.c.  Each atlas carries an opaque white texel
       (appended row) that solid-color draws sample, so no separate white texture is needed --
       solids and text share the atlas and merge into one draw. */
    if ( !font_init() )
    {
        rhi()->unregister_sampler( s_render.font_sampler_idx );
        rhi()->sampler_destroy( s_render.font_sampler );
        rhi()->pipeline_destroy( s_render.pipeline );
        return false;
    }

    return true;
}

void
imgui_render_shutdown( void )
{
    // Peak draw-list usage over the run, so the caps can be tuned with real numbers.
    printf( "[imgui] peak draw-list usage: verts %u/%u (%.1f%%), idx %u/%u (%.1f%%)%s\n",
            s_tess.vert_hwm, IMGUI_MAX_VERTS, 100.0f * s_tess.vert_hwm / (f32)IMGUI_MAX_VERTS,
            s_tess.idx_hwm,  IMGUI_MAX_IDX,   100.0f * s_tess.idx_hwm  / (f32)IMGUI_MAX_IDX,
            s_tess.overflow_ever ? "  -- OVERFLOWED (geometry was dropped)" : "" );

    // Peak draw calls in a single frame -- a measure of batching effectiveness.
    printf( "[imgui] peak draw calls in a frame: %u\n", cache_draw_call_hwm() );

    font_shutdown();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );

    if ( rhi_handle_valid( s_render.pipeline_wire ) )
        rhi()->pipeline_destroy( s_render.pipeline_wire );
    if ( rhi_handle_valid( s_render.pipeline ) )
        rhi()->pipeline_destroy( s_render.pipeline );

    // Per-viewport geometry buffers are released by viewport_destroy (driven from imgui_shutdown).
    memset( &s_render, 0, sizeof( s_render ) );
}

/*==============================================================================================
    Memory stats
==============================================================================================*/

// imgui_render_memory -- GPU resource memory held by imgui (bytes).  Buffers are sized at init and
// fixed; texture_bytes reflects the currently initialized atlases (each includes its white row).
imgui_mem_stats_t
imgui_render_memory( void )
{
    imgui_mem_stats_t s;
    s.vertex_bytes  = RHI_MAX_FRAMES_IN_FLIGHT * (u32)IMGUI_VB_REGION_BYTES;
    s.index_bytes   = RHI_MAX_FRAMES_IN_FLIGHT * (u32)IMGUI_IB_REGION_BYTES;
    s.texture_bytes = font_atlas_bytes();
    s.total_bytes   = s.vertex_bytes + s.index_bytes + s.texture_bytes;
    return s;
}

// imgui_render_print_memory -- dump the breakdown to stdout (one line per bucket).
void
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

/*==============================================================================================
    Debug render mode -- the debug view (normal / wireframe / batch-tint).

    Cheap to flip every frame; read by imgui_render_flush.  Backs imgui()->debug_set/get_render_mode.
==============================================================================================*/

void
imgui_render_set_mode( imgui_render_mode_t mode )
{
    if ( mode < 0 || mode >= IMGUI_RENDER_MODE_COUNT )
        mode = IMGUI_RENDER_NORMAL;
    s_render.debug_mode = mode;
}

imgui_render_mode_t
imgui_render_get_mode( void )
{
    return s_render.debug_mode;
}

/* batch_debug_color -- a distinct, saturated, fully-opaque color per draw-call index for the BATCH
   view.  Packed RGBA8 (R low byte), matching the shader's dbg_tint decode and IMGUI_COLOR byte order.
   A 12-entry table cycles; consecutive entries are spread around the hue wheel so neighbouring
   batches stay easy to tell apart, and the wrap is harmless (it only marks boundaries). */
static u32
batch_debug_color( u32 i )
{
    static const u32 palette[ 12 ] = {
        IMGUI_COLOR( 0xE6, 0x39, 0x46, 0xFF ),   // red
        IMGUI_COLOR( 0x2A, 0x9D, 0x8F, 0xFF ),   // teal
        IMGUI_COLOR( 0xE9, 0xC4, 0x6A, 0xFF ),   // yellow
        IMGUI_COLOR( 0x45, 0x7B, 0x9D, 0xFF ),   // blue
        IMGUI_COLOR( 0xF4, 0x7A, 0x20, 0xFF ),   // orange
        IMGUI_COLOR( 0x8E, 0x44, 0xAD, 0xFF ),   // purple
        IMGUI_COLOR( 0x6A, 0xBE, 0x30, 0xFF ),   // green
        IMGUI_COLOR( 0xD6, 0x4B, 0x9C, 0xFF ),   // pink
        IMGUI_COLOR( 0x34, 0x98, 0xDB, 0xFF ),   // sky
        IMGUI_COLOR( 0xC0, 0x8A, 0x3E, 0xFF ),   // brown
        IMGUI_COLOR( 0x1A, 0xBC, 0x9C, 0xFF ),   // mint
        IMGUI_COLOR( 0xBD, 0xC3, 0xC7, 0xFF ),   // silver
    };
    return palette[ i % 12u ];
}

/*==============================================================================================
    imgui_render_flush -- upload one surface's geometry and emit its draw calls (SUBMIT phase).

    Paints into `vp` (the surface at index `vp_index`).  First kicks the once-per-frame BUILD
    (cache_build_frame, lazy -- only the first surface this frame pays for it; the rest reuse the
    result).  Then uploads this surface's slice of the shared geometry into vp's own vb/ib region and
    opens a LOAD pass on vp->target, so a surface's geometry and target travel together.  The host
    calls this once per live surface with that surface's drawable size; slots tagged for another
    viewport are skipped (their index range still stepped over to keep first_index aligned).

    Geometry is shared and indices are slot-local (vertex_offset = slot->vert_base shifts them to the
    absolute VB position), so the whole vertex/index list is uploaded to every surface's buffer and
    each surface draws only its own slots.  LOAD preserves the scene rendered before this call; each
    draw command applies its own scissor.
==============================================================================================*/

void
imgui_render_flush( imgui_viewport_t* vp, u32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( s_draw.cmd_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    // Tessellate + sort the shared list once per frame; this surface reuses the cached result.
    cache_build_frame();

    // Select this frame's geometry region so the upload cannot clobber data the GPU is still reading
    // for another in-flight frame.
    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = frame * (u32)IMGUI_VB_REGION_BYTES;
    u32 ib_off = frame * (u32)IMGUI_IB_REGION_BYTES;

    /* This surface's vertex + index upload span, taken from the slot table.  Slots tagged for this
       viewport contribute their [vert_base, +count) and [idx_base, +count) ranges; the union is what
       we upload.  For a single surface this covers the whole buffer. */
    u32 vtx_lo = s_tess.vert_count, vtx_hi = 0;
    u32 idx_lo = s_tess.idx_count,  idx_hi = 0;
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* sl = s_dispatch[ d ];
        if ( sl->vp != vp_index || sl->vert_count == 0 ) continue;
        if ( sl->vert_base                     < vtx_lo ) vtx_lo = sl->vert_base;
        if ( sl->vert_base + sl->vert_count    > vtx_hi ) vtx_hi = sl->vert_base + sl->vert_count;
        if ( sl->idx_base                      < idx_lo ) idx_lo = sl->idx_base;
        if ( sl->idx_base  + sl->idx_count     > idx_hi ) idx_hi = sl->idx_base  + sl->idx_count;
    }

    if ( vtx_hi > vtx_lo )
        rhi()->buffer_write( vp->vb,
                             &s_tess.verts[ vtx_lo ],
                             ( vtx_hi - vtx_lo ) * sizeof( imgui_draw_vert_t ),
                             vb_off + vtx_lo * (u32)sizeof( imgui_draw_vert_t ) );
    if ( idx_hi > idx_lo )
        rhi()->buffer_write( vp->ib,
                             &s_tess.indices[ idx_lo ],
                             ( idx_hi - idx_lo ) * sizeof( u16 ),
                             ib_off + idx_lo * (u32)sizeof( u16 ) );

    /* Open a LOAD pass on the swapchain color target (no depth).  LOAD preserves the scene content
       rendered before this call; CLEAR would wipe it. */
    rhi_color_attachment_t color_att = {
        .texture  = vp->target,
        .load_op  = RHI_LOAD_OP_LOAD,
        .store_op = RHI_STORE_OP_STORE,
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    // Full-window viewport (dynamic state; must be set before the first draw).
    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f,
        .width     = (f32)win_w,
        .height    = (f32)win_h,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    } );

    // Wireframe mode binds the LINE pipeline (falling back to fill if it failed to compile); normal +
    // batch modes both rasterize filled triangles.
    rhi_pipeline_t pipe = ( s_render.debug_mode == IMGUI_RENDER_WIREFRAME
                            && rhi_handle_valid( s_render.pipeline_wire ) )
                        ? s_render.pipeline_wire : s_render.pipeline;
    rhi()->cmd_bind_pipeline( cmd, pipe );
    rhi()->cmd_bind_bindless( cmd );
    // Bind this frame's region; index values and first_index stay region-relative.
    rhi()->cmd_bind_vertex_buffer( cmd, vp->vb, vb_off );
    rhi()->cmd_bind_index_buffer( cmd, vp->ib, ib_off, RHI_INDEX_TYPE_UINT16 );

    // Ortho matrix: pixel [0,w]x[0,h] -> NDC [-1,+1]x[-1,+1].
    imgui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );
    push.samp_idx = s_render.font_sampler_idx;

    /* Debug render-mode push state.  dbg_flat makes the fragment bypass the atlas and emit a flat
       color: WIREFRAME keeps each window's vertex color (tint 0), BATCH overrides it per draw call
       with a palette color below.  NORMAL leaves both 0 for the textured/blended path. */
    bool batch_view = ( s_render.debug_mode == IMGUI_RENDER_BATCH );
    push.dbg_flat   = ( s_render.debug_mode == IMGUI_RENDER_NORMAL ) ? 0u : 1u;
    push.dbg_tint   = 0u;

    u32 draw_calls = 0;   // indexed draws actually emitted this surface (one per non-empty command)

    /* Walk s_dispatch[] (z-sorted slot pointers) back-to-front.  Each slot owns a contiguous region
       of s_tess.verts[]/indices[]; its GPU commands reference those via 0-relative indices +
       vertex_offset = slot->vert_base.  Slots for other viewports are skipped entirely; within a
       slot, a command with a mismatched vp is stepped over (its index range still consumed to keep
       first_index aligned). */
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* slot = s_dispatch[ d ];
        if ( slot->vp != vp_index )
            continue;

        u32 first_index = slot->idx_base;   // absolute start of this slot's indices in the IB
        for ( u32 k = 0; k < slot->cmd_count; ++k )
        {
            u32                    ci = slot->cmd_base + k;
            const imgui_gpu_cmd_t* dc = &s_tess.cmds[ ci ];

            if ( s_tess.cmd_vp[ ci ] != vp_index )
            {
                first_index += dc->elem_count;
                continue;
            }
            if ( dc->elem_count == 0 )
                continue;

            // Scissor to the command's clip rect.  Floor the origin and ceil the far edge so a
            // fractional clip never rounds inward and shaves a pixel off visible content.
            i32 sx0 = (i32)floorf( dc->clip_rect.x );
            i32 sy0 = (i32)floorf( dc->clip_rect.y );
            i32 sx1 = (i32)ceilf ( dc->clip_rect.x + dc->clip_rect.w );
            i32 sy1 = (i32)ceilf ( dc->clip_rect.y + dc->clip_rect.h );

            // Clamp to framebuffer bounds (Vulkan requires offset >= 0 and extent within the surface).
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

            push.tex_idx = dc->tex_idx;
            if ( batch_view )
                push.dbg_tint = batch_debug_color( draw_calls );
            rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

            rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
                .index_count    = dc->elem_count,
                .instance_count = 1,
                .first_index    = first_index,
                .vertex_offset  = (i32)slot->vert_base,   // slot-local indices + vert_base = absolute
                .first_instance = 0,
            } );
            ++draw_calls;

            first_index += dc->elem_count;
        }
    }

    rhi()->cmd_end_rendering( cmd );

    // Fold this surface's draw-call count into the frame accumulator + lifetime peak (cache stats).
    cache_count_draw_calls( draw_calls );

    static u32 prev_draw_calls = ~0u;   // sentinel: forces a print on the first frame
    if ( s_dbg_draw_calls && draw_calls != prev_draw_calls )
    {
        printf( "[imgui] draw calls this frame: %u (peak %u)\n", draw_calls, cache_draw_call_hwm() );
        prev_draw_calls = draw_calls;
    }
}

// clang-format on
/*============================================================================================*/
