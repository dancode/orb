/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_submit.c -- GPU resources + draw submission (RENDER phase).

    The last of the three render phases (see gui_build_cache.c for the full map):

        EMIT    gui_emit_draw.c    widgets -> s_draw semantic command list
        BUILD   gui_build_cache.c  diff + tessellate -> s_tess geometry + s_dispatch slot table
        RENDER  this file          upload each surface's slots + emit indexed draw calls

    Two responsibilities live here:

      - GPU resources (render_init / _shutdown): the shared pipeline + font sampler, created
        once; and a surface's own vertex/index buffers (surface_geo_create / surface_geo_destroy),
        created per render target.  These are immutable across frames and shared by every surface.

      - The flush (gui_render_flush): kick the once-per-frame BUILD (cache_build_frame, lazy), then
        upload this surface's slice of the shared geometry and emit one indexed draw call per cached
        GPU command, back-to-front in dispatch order.  The flush takes the surface's GPU pieces
        (vb / ib / target) as PARAMETERS -- the surface RECORD (gui_viewport_t) is the interact
        server's storage, orchestrated by frame, and this server never sees it (R11).

    Included by gui_render.c after gui_build_cache.c (cache_build_frame, s_dispatch, the slot
    types, the stats accessors) -- which in turn follows gui_build_tess.c (s_tess) and
    gui_emit_draw.c (s_draw).  gui_debug_overlay.c follows this file and reuses s_render + render_ortho.

==============================================================================================*/
#include "engine/sys/sys_host.h"                // sys_exe_dir -- probe for cooked .oshd shaders
                                                //   (gui is a static lib: sys is always in the host)

// clang-format off
/*==============================================================================================
    Push constant layout (84 bytes; must match gui_shader.h GLSL source)
==============================================================================================*/

typedef struct
{
    f32 mvp[ 16 ];      // column-major ortho matrix       64 bytes
    u32 tex_idx;        // bindless texture slot            4 bytes
    u32 samp_idx;       // bindless sampler slot            4 bytes
    u32 dbg_flat;       // debug: 1 = flat color (no atlas) 4 bytes
    u32 dbg_tint;       // debug: packed RGBA8 batch tint   4 bytes
    u32 rgba_tex;       // 1 = full-RGBA image sampling     4 bytes

} gui_push_t;         // total 84 bytes -- well within RHI_MAX_PUSH_CONST_SIZE

/*==============================================================================================
    Per-frame geometry regions.

    The CPU records up to RHI_MAX_FRAMES_IN_FLIGHT frames ahead of the GPU, so each surface's VB/IB
    holds one independent region per in-flight slot.  Each frame writes and binds only its own region
    (selected by cmd_frame_index), so this frame's upload never overwrites geometry the GPU is still
    reading for a previous in-flight frame.
==============================================================================================*/

#define GUI_VB_REGION_BYTES  ( GUI_MAX_VERTS * sizeof( gui_draw_vert_t ) )
#define GUI_IB_REGION_BYTES  ( GUI_MAX_IDX   * sizeof( u16 ) )

/*==============================================================================================
    Shared GPU resources -- created once in render_init, destroyed in render_shutdown.

    Immutable across frames and shared by every viewport (and the debug overlay), so never a
    per-viewport or per-frame bottleneck.  Per-viewport surfaces own only their vb/ib (in
    gui_viewport_t, core/gui_ctx.h); a viewport is a render TARGET that windows are dispatched to, not an owner of
    windows -- the one context emits every window and flush routes each window's geometry to the
    viewport hosting it.  The viewport list lives in the bound context (core/gui_ctx.c), so
    this file only ever touches a surface through the GPU pieces passed to it.
==============================================================================================*/

static struct
{
    rhi_pipeline_t  pipeline;           // compiled pipeline: gui shaders + vertex layout + alpha blend
    rhi_pipeline_t  pipeline_wire;      // same pipeline in VK_POLYGON_MODE_LINE (wireframe debug view)
    rhi_sampler_t   font_sampler;       // sampler for font textures (point clamp)
    u32             font_sampler_idx;   // bindless slot for font_sampler

    gui_render_mode_t debug_mode;     // NORMAL / WIREFRAME / BATCH -- how the UI list is rasterized

} s_render;

/*==============================================================================================
    render_ortho -- column-major pixel-space orthographic matrix.

    Maps pixel coords ([0,w] x [0,h], origin top-left) to Vulkan NDC:
        x: [0,w] -> [-1,+1]   y: [0,h] -> [-1,+1]  (top-left is -1,-1 in Vulkan NDC)
==============================================================================================*/

static void
render_ortho( f32 out[ 16 ], f32 w, f32 h )
{
    out[  0 ] =  2.0f / w; out[  1 ] =  0.0f;     out[  2 ] = 0.0f; out[  3 ] = 0.0f;
    out[  4 ] =  0.0f;     out[  5 ] =  2.0f / h; out[  6 ] = 0.0f; out[  7 ] = 0.0f;
    out[  8 ] =  0.0f;     out[  9 ] =  0.0f;     out[ 10 ] = 1.0f; out[ 11 ] = 0.0f;
    out[ 12 ] = -1.0f;     out[ 13 ] = -1.0f;     out[ 14 ] = 0.0f; out[ 15 ] = 1.0f;
}

/*==============================================================================================
    Per-surface geometry ring -- a surface's own GPU geometry buffers.

    Per surface so each has an independent vb/ib ring (one region per frame-in-flight).  Called
    once per surface by the orchestrator's viewport_create/destroy (frame/gui_viewport.c), which
    own every non-GPU field of the surface record -- this server only mints and frees the pair.
    The shared pipeline / sampler / atlas are NOT here -- those are created once in render_init.
==============================================================================================*/

bool
surface_geo_create( rhi_buffer_t* vb, rhi_buffer_t* ib )
{
    // Vertex buffer (CPU_TO_GPU): one region per frame-in-flight, written every frame.
    *vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * GUI_VB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_vb",
    } );
    if ( !rhi_handle_valid( *vb ) )
        return false;

    // Index buffer (CPU_TO_GPU, u16 indices): one region per frame-in-flight.
    *ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * GUI_IB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_INDEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_ib",
    } );
    if ( !rhi_handle_valid( *ib ) )
    {
        rhi()->buffer_destroy( *vb );
        return false;
    }

    return true;
}

void
surface_geo_destroy( rhi_buffer_t* vb, rhi_buffer_t* ib )
{
    if ( rhi_handle_valid( *ib ) ) rhi()->buffer_destroy( *ib );
    if ( rhi_handle_valid( *vb ) ) rhi()->buffer_destroy( *vb );

    *vb = ( rhi_buffer_t ){ 0 };
    *ib = ( rhi_buffer_t ){ 0 };
}

/*==============================================================================================
    Init / shutdown -- the shared GPU resources (pipeline, font sampler, atlas).
==============================================================================================*/

/*==============================================================================================
    render_try_oshd_shaders -- the OPTIONAL cooked-shader path.

    If cook_shaders.bat has produced bin/shaders/gui.{vs,ps}.oshd next to the exe (cooked from
    shaders/gui.{vs,ps}.hlsl), load that pair instead of the embedded arrays -- the containers
    carry reflection, so pipeline_create validates the vertex layout and push range against the
    actual SPIR-V.  All-or-nothing: both files must exist and load, or the caller falls back to
    the embedded fallback in gui_shader.h.  Absent files are NOT an error -- the cooked path is
    additive, never a dependency (delete bin/shaders to turn it off).
==============================================================================================*/

static bool
render_try_oshd_shaders( rhi_shader_t* out_vert, rhi_shader_t* out_frag )
{
    char dir[ 512 ];
    sys_exe_dir( dir, ( int )sizeof( dir ) );

    char vs_path[ 576 ], ps_path[ 576 ];
    fmt_snprintf( vs_path, sizeof( vs_path ), "%s/shaders/gui.vs.oshd", dir );
    fmt_snprintf( ps_path, sizeof( ps_path ), "%s/shaders/gui.ps.oshd", dir );

    /* Probe with fopen first so a missing pair stays silent (the normal fallback case);
       shader_load_oshd would LOG_ERROR on a missing file. */
    FILE* fv = fopen( vs_path, "rb" );
    FILE* fp = fopen( ps_path, "rb" );
    if ( fv ) fclose( fv );
    if ( fp ) fclose( fp );
    if ( !fv || !fp )
        return false;

    rhi_shader_t vert = rhi()->shader_load_oshd( vs_path, "gui_vert(oshd)" );
    if ( !rhi_handle_valid( vert ) )
        return false;

    rhi_shader_t frag = rhi()->shader_load_oshd( ps_path, "gui_frag(oshd)" );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
        return false;
    }

    *out_vert = vert;
    *out_frag = frag;
    return true;
}

static bool
render_init( void )
{
    /* Cooked .oshd pair when present, embedded SPIR-V otherwise (see render_try_oshd_shaders). */
    rhi_shader_t vert = { RHI_NULL_HANDLE };
    rhi_shader_t frag = { RHI_NULL_HANDLE };

    if ( render_try_oshd_shaders( &vert, &frag ) )
    {
        printf( "[gui] using cooked shaders (bin/shaders/gui.{vs,ps}.oshd)\n" );
    }
    else
    {
        vert = rhi()->shader_load_memory(
            s_gui_vert_spirv, sizeof( s_gui_vert_spirv ),
            RHI_SHADER_STAGE_VERTEX, "main", "gui_vert" );
        if ( !rhi_handle_valid( vert ) )
            return false;

        frag = rhi()->shader_load_memory(
            s_gui_frag_spirv, sizeof( s_gui_frag_spirv ),
            RHI_SHADER_STAGE_FRAGMENT, "main", "gui_frag" );
        if ( !rhi_handle_valid( frag ) )
        {
            rhi()->shader_destroy( vert );
            return false;
        }
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
        .vertex_stride      = sizeof( gui_draw_vert_t ),
        .cull               = RHI_CULL_NONE,
        .polygon_mode       = RHI_POLYGON_FILL,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( gui_push_t ),
        .debug_name         = "gui",
    };
    s_render.pipeline = rhi()->pipeline_create( &pdesc );

    /* Wireframe pipeline: the debug-view (normal / wireframe / batch-tint) toggle's only extra
       GPU resource.  gui_render_flush falls back to the fill pipeline if this one ever fails to
       create (see the fallback below). */
    pdesc.polygon_mode = RHI_POLYGON_LINE;
    pdesc.debug_name   = "gui_wire";
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

    /* Fonts boot in the DRAW unit now (gui_draw_boot, after the whole server stands up) --
       the shared atlas carries the opaque white texel solid-color draws sample, so solids
       and text still share one texture and merge into one draw. */
    return true;
}

static void
render_shutdown( void )
{
    // Peak draw-list usage over the run, so the caps can be tuned with real numbers.
    printf( "[gui] peak draw-list usage: verts %u/%u (%.1f%%), idx %u/%u (%.1f%%)%s\n",
            s_tess_stats.vert_hwm, GUI_MAX_VERTS, 100.0f * s_tess_stats.vert_hwm / (f32)GUI_MAX_VERTS,
            s_tess_stats.idx_hwm,  GUI_MAX_IDX,   100.0f * s_tess_stats.idx_hwm  / (f32)GUI_MAX_IDX,
            s_tess_stats.overflow_ever ? "  -- OVERFLOWED (geometry was dropped)" : "" );

    // Peak draw calls in a single frame -- a measure of batching effectiveness.
    printf( "[gui] peak draw calls in a frame: %u\n", cache_draw_call_hwm() );

    /* The last submitted frame may still be executing on the GPU; the destroys below
       (font textures, samplers, pipelines) are immediate, so drain the device first. */
    rhi()->device_wait_idle();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );

    if ( rhi_handle_valid( s_render.pipeline_wire ) )
        rhi()->pipeline_destroy( s_render.pipeline_wire );
    if ( rhi_handle_valid( s_render.pipeline ) )
        rhi()->pipeline_destroy( s_render.pipeline );

    // Per-viewport geometry buffers are released by viewport_destroy (driven from gui_shutdown).
    memset( &s_render, 0, sizeof( s_render ) );
}

/* Memory accounting lives in render/gui_render_mem.c (backend_memory) -- the LAST include
   of the unity TU, so it can sizeof every backend static, including the capture/debug files
   included after this one. */

/*==============================================================================================
    Debug render mode -- the debug view (normal / wireframe / batch-tint).

    Cheap to flip every frame; read by gui_render_flush.  Backs gui()->debug_set/get_render_mode.
==============================================================================================*/

void
gui_render_set_mode( gui_render_mode_t mode )
{
    if ( mode < 0 || mode >= GUI_RENDER_MODE_COUNT )
        mode = GUI_RENDER_NORMAL;
    s_render.debug_mode = mode;
}

gui_render_mode_t
gui_render_get_mode( void )
{
    return s_render.debug_mode;
}

/* render_batch_debug_color -- a distinct, saturated, fully-opaque color per draw-call index for the BATCH
   view.  Packed RGBA8 (R low byte), matching the shader's dbg_tint decode and GUI_COLOR byte order.
   A 12-entry table cycles; consecutive entries are spread around the hue wheel so neighbouring
   batches stay easy to tell apart, and the wrap is harmless (it only marks boundaries). */
static u32
render_batch_debug_color( u32 i )
{
    static const u32 palette[ 12 ] = {
        GUI_COLOR( 0xE6, 0x39, 0x46, 0xFF ),   // red
        GUI_COLOR( 0x2A, 0x9D, 0x8F, 0xFF ),   // teal
        GUI_COLOR( 0xE9, 0xC4, 0x6A, 0xFF ),   // yellow
        GUI_COLOR( 0x45, 0x7B, 0x9D, 0xFF ),   // blue
        GUI_COLOR( 0xF4, 0x7A, 0x20, 0xFF ),   // orange
        GUI_COLOR( 0x8E, 0x44, 0xAD, 0xFF ),   // purple
        GUI_COLOR( 0x6A, 0xBE, 0x30, 0xFF ),   // green
        GUI_COLOR( 0xD6, 0x4B, 0x9C, 0xFF ),   // pink
        GUI_COLOR( 0x34, 0x98, 0xDB, 0xFF ),   // sky
        GUI_COLOR( 0xC0, 0x8A, 0x3E, 0xFF ),   // brown
        GUI_COLOR( 0x1A, 0xBC, 0x9C, 0xFF ),   // mint
        GUI_COLOR( 0xBD, 0xC3, 0xC7, 0xFF ),   // silver
    };
    return palette[ i % 12u ];
}

/*==============================================================================================
    gui_render_flush -- upload one surface's geometry and emit its draw calls (SUBMIT phase).

    Paints into the surface at index `vp_index`, whose GPU pieces (vb / ib / target) arrive as
    parameters -- the orchestrator (frame/gui_frame_loop.c) passes the record's fields; this server
    never sees the record.  First kicks the once-per-frame BUILD (cache_build_frame, lazy --
    only the first surface this frame pays for it; the rest reuse the result).  Then uploads
    this surface's slice of the shared geometry into its own vb/ib region and opens a LOAD pass
    on the target, so a surface's geometry and target travel together.  The host calls this
    once per live surface with that surface's drawable size; slots tagged for another viewport
    are skipped (each command carries its own first_index in s_tess.gpu_cmds[].ibase).

    Geometry is shared and indices are slot-local (vertex_offset = slot->vert_base shifts them to the
    absolute VB position), so the whole vertex/index list is uploaded to every surface's buffer and
    each surface draws only its own slots.  LOAD preserves the scene rendered before this call; each
    draw command applies its own scissor.
==============================================================================================*/

void
gui_render_flush( rhi_buffer_t vb, rhi_buffer_t ib, rhi_texture_t target,
                  u32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( s_draw.cmd_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    // Tessellate + sort the shared list once per frame; this surface reuses the cached result.
    cache_build_frame();

    // Select this frame's geometry region so the upload cannot clobber data the GPU is still reading
    // for another in-flight frame.
    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = frame * (u32)GUI_VB_REGION_BYTES;
    u32 ib_off = frame * (u32)GUI_IB_REGION_BYTES;

    /* This surface's vertex + index upload span, taken from the slot table.  Slots tagged for this
       viewport contribute their [vert_base, +count) and [idx_base, +count) ranges; the union is what
       we upload.  For a single surface this covers the whole buffer. */
    u32 vtx_lo = s_tess.vert_count, vtx_hi = 0;
    u32 idx_lo = s_tess.idx_count,  idx_hi = 0;

    u32 overlay_bytes = 0;   // debug-band windows' share, excluded from the upload stats

    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* sl = s_dispatch[ d ];
        if ( sl->vp != vp_index || sl->vert_count == 0 ) continue;

        if ( sl->band != 0 )
        {
            overlay_bytes += sl->vert_count * sizeof( gui_draw_vert_t );
            overlay_bytes += sl->idx_count * sizeof( u16 );
        }

        if ( sl->vert_base                     < vtx_lo ) vtx_lo = sl->vert_base;
        if ( sl->vert_base + sl->vert_count    > vtx_hi ) vtx_hi = sl->vert_base + sl->vert_count;
        if ( sl->idx_base                      < idx_lo ) idx_lo = sl->idx_base;
        if ( sl->idx_base  + sl->idx_count     > idx_hi ) idx_hi = sl->idx_base  + sl->idx_count;
    }

    u32 up_batches = 0;
    u32 up_bytes = 0;

    if ( vtx_hi > vtx_lo )
    {
        u32 bytes = ( vtx_hi - vtx_lo ) * sizeof( gui_draw_vert_t );
        rhi()->buffer_write( vb,
                             &s_tess.verts[ vtx_lo ],
                             bytes,
                             vb_off + vtx_lo * (u32)sizeof( gui_draw_vert_t ) );
        up_batches++;
        up_bytes += bytes;
    }
    if ( idx_hi > idx_lo )
    {
        u32 bytes = ( idx_hi - idx_lo ) * sizeof( u16 );
        rhi()->buffer_write( ib,
                             &s_tess.indices[ idx_lo ],
                             bytes,
                             ib_off + idx_lo * (u32)sizeof( u16 ) );
        up_batches++;
        up_bytes += bytes;
    }

    if ( up_batches > 0 )
    {
        u32 actual_bytes = up_bytes > overlay_bytes ? up_bytes - overlay_bytes : 0;
        u32 actual_batches = actual_bytes > 0 ? up_batches : 0;
        cache_count_upload( actual_batches, actual_bytes );
    }

    /* Open a LOAD pass on the swapchain color target (no depth).  LOAD preserves the scene content
       rendered before this call; CLEAR would wipe it. */
    rhi_color_attachment_t color_att = {
        .texture  = target,
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
    rhi_pipeline_t pipe = ( s_render.debug_mode == GUI_RENDER_WIREFRAME
                            && rhi_handle_valid( s_render.pipeline_wire ) )
                        ? s_render.pipeline_wire : s_render.pipeline;
    rhi()->cmd_bind_pipeline( cmd, pipe );
    rhi()->cmd_bind_bindless( cmd );
    // Bind this frame's region; index values and first_index stay region-relative.
    rhi()->cmd_bind_vertex_buffer( cmd, vb, vb_off );
    rhi()->cmd_bind_index_buffer( cmd, ib, ib_off, RHI_INDEX_TYPE_UINT16 );

    // Ortho matrix: pixel [0,w]x[0,h] -> NDC [-1,+1]x[-1,+1].
    gui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );
    push.samp_idx = s_render.font_sampler_idx;

    /* Debug render-mode push state.  dbg_flat makes the fragment bypass the atlas and emit a flat
       color: WIREFRAME keeps each window's vertex color (tint 0), BATCH overrides it per draw call
       with a palette color below.  NORMAL leaves both 0 for the textured/blended path. */
    bool batch_view = ( s_render.debug_mode == GUI_RENDER_BATCH );
    push.dbg_flat   = ( s_render.debug_mode == GUI_RENDER_NORMAL ) ? 0u : 1u;
    push.dbg_tint   = 0u;

    u32 draw_calls = 0;   // indexed draws actually emitted this surface (one per non-empty command)

    /* Walk s_dispatch[] (z-sorted slot pointers) back-to-front.  Each slot owns a contiguous region
       of s_tess.verts[]/indices[]; its GPU commands reference those via 0-relative indices +
       vertex_offset = slot->vert_base.  Slots for other viewports are skipped entirely, as are
       commands with a mismatched vp (including a volatile block's dormant reserved commands,
       tagged GUI_VP_INVALID).  first_index comes straight off each command's own ibase --
       explicit rather than accumulated, since the index buffer may contain reserved headroom gaps
       between a volatile block's live indices and the commands that follow it. */
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* slot = s_dispatch[ d ];
        if ( slot->vp != vp_index )
            continue;

        for ( u32 k = 0; k < slot->cmd_count; ++k )
        {
            u32                    ci = slot->cmd_base + k;
            const tess_gpu_cmd_t* gc = &s_tess.gpu_cmds[ ci ];
            const gui_gpu_cmd_t*  dc = &gc->cmd;

            if ( gc->vp != vp_index )
                continue;
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

            push.tex_idx  = dc->tex_idx & ~GUI_TEX_RGBA_BIT;
            push.rgba_tex = ( dc->tex_idx & GUI_TEX_RGBA_BIT ) ? 1u : 0u;
            if ( batch_view )
                push.dbg_tint = render_batch_debug_color( draw_calls );
            rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

            rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
                .index_count    = dc->elem_count,
                .instance_count = 1,
                .first_index    = gc->ibase,
                .vertex_offset  = (i32)slot->vert_base,   // slot-local indices + vert_base = absolute
                .first_instance = 0,
            } );

            if ( slot->band == 0 )   /* debug-band draws never count in the stats they display */
                ++draw_calls;
        }
    }

    rhi()->cmd_end_rendering( cmd );

    // Fold this surface's draw-call count into the frame accumulator + lifetime peak (cache stats).
    cache_count_draw_calls( draw_calls );

    /* Pipeline-dashboard flush capture: what physically hit the GPU for this surface (frame
       region, upload spans, bytes/writes, draws).  A no-op unless GUI_PIPELINE_DASHBOARD. */
    DASH_CAPTURE_FLUSH( vp_index, frame, vtx_lo, vtx_hi, idx_lo, idx_hi,
                        up_bytes, up_batches, draw_calls );
}

// clang-format on
/*============================================================================================*/
