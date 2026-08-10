/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_render_init.c -- Shared GPU resources

    The RENDER phase's one-time setup, shared by every surface for the life of the run: the
    compiled pipeline (+ wireframe twin), the two bindless samplers (font/coverage + image), and
    the push-constant layout they're built against.  render_init creates them; render_shutdown
    tears them down and logs the run's peak draw-list / draw-call / per-draw-state figures.

        EMIT    gui_emit_draw.c    widgets -> s_draw semantic command list
        BUILD   gui_build_cache.c  diff + tessellate -> s_tess geometry + s_dispatch slot table
        RENDER  this file          shared GPU resources (once) -- gui_submit.c submits per surface

    Per-surface state -- a surface's own geometry buffers and the flush that uploads and draws
    them -- lives next door in pipeline/gui_submit.c, which reads s_render (this file's static)
    as a shared constant and never writes it.

    Included by gui_render.c right after gui_build_cache.c and before gui_submit.c, which depends
    on the gui_push_t layout and s_render defined here.

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
    u32 samp_point;     // bindless sampler: NEAREST        4 bytes
    u32 samp_image;     // bindless sampler: LINEAR         4 bytes
    u32 dbg_flat;       // debug: 1 = flat color (no atlas) 4 bytes
    u32 dbg_tint;       // debug: packed RGBA8 batch tint   4 bytes
    f32 time;           // frame clock, wrapped seconds     4 bytes

} gui_push_t;         // total 84 bytes -- well within RHI_MAX_PUSH_CONST_SIZE

/*  The texture and its sampling model USED to live here, one pair per draw call, and that is
    exactly what forced a draw call per texture.  They moved into the vertex (gui.h): the fragment
    reads the slot from the vertex and picks between the two SAMPLERS below by model, so what
    remains in this block is per-FRAME state only.  In normal rendering nothing in the tail changes
    across a whole flush -- the redundancy filter below pushes it once and then goes quiet.  */

/*  The whole block is written ONCE per flush, before the dispatch walk -- everything in it is
    frame-constant now.  The tail offset below survives for the ONE consumer that still rewrites
    mid-walk: the BATCH debug view, whose per-draw tint is deliberately different per draw call
    (see gui_render_flush).

    Vulkan leaves push constants undefined until written, and the head is written after this
    flush's cmd_bind_pipeline, so a scene pass that bound its own pipeline earlier in the command
    buffer cannot leave a stale matrix behind.

    Derived with offsetof rather than spelled 64/24: the struct is free to be reordered pre-ship
    (CLAUDE.md), and the assert below is what makes that safe -- the split is only valid while the
    mvp is first and the tail is everything after it.  */
#define GUI_PUSH_TAIL_OFF   ( (u32)offsetof( gui_push_t, samp_point ) )
#define GUI_PUSH_TAIL_SIZE  ( (u32)( sizeof( gui_push_t ) - offsetof( gui_push_t, samp_point ) ) )

ORB_STATIC_ASSERT( offsetof( gui_push_t, mvp ) == 0,
                   "the mvp must lead gui_push_t -- the per-draw push writes everything after it" );
ORB_STATIC_ASSERT( GUI_PUSH_TAIL_OFF == sizeof( ( (gui_push_t*)0 )->mvp ),
                   "gui_push_t tail must start immediately after the mvp" );
ORB_STATIC_ASSERT( GUI_PUSH_TAIL_OFF % 4 == 0 && GUI_PUSH_TAIL_SIZE % 4 == 0,
                   "vkCmdPushConstants requires 4-byte aligned offset and size" );

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
    rhi_sampler_t   font_sampler;       // coverage sampler: point, U repeats (dash tiling), V clamps
    u32             font_sampler_idx;   // bindless slot for font_sampler

    /* Second sampler, bilinear, for the sampling models that filter: authored sprite art, a
       caller's own textures (a scene render target), and distance-field glyphs.  BOTH slots are
       pushed every flush and the FRAGMENT chooses between them from the vertex's model field, so a
       draw call binds neither in particular -- which is exactly what lets one batch mix a point-
       sampled glyph atlas with a filtered image.  The model answers the question on its own:
       coverage is glyphs and icons, which must stay point-sampled to render crisp, and everything
       else is a picture or a field, which must filter. */
    rhi_sampler_t   image_sampler;      // sampler for RGBA images (bilinear clamp)
    u32             image_sampler_idx;  // bindless slot for image_sampler

    gui_render_mode_t debug_mode;     // NORMAL / WIREFRAME / BATCH -- how the UI list is rasterized

    /* The frame clock handed down by the orchestrator (gui_render_set_time), already wrapped.
       Held here rather than read from the IO snapshot because s_io lives in the frontend unit and
       this server cannot see it -- the same one-way seam gui_render_set_mode crosses. */
    f32 fx_time;                      // seconds since the first frame, wrapped; -> pc.time

    /* Lifetime per-draw-state totals from gui_render_flush, reported at shutdown.  The scissor
       filter is invisible by construction -- it changes no pixel -- so without a count there is no
       way to tell whether it is earning anything on a real UI.  The push figure is no longer a
       filter yield but a cost: the tail is per-FLUSH now, so state_flushes is its denominator. */
    u64 state_draws;                  // draw calls walked (the scissor filter's denominator)
    u64 state_flushes;                // surface flushes walked (the push denominator)
    u64 state_pushes;                 // push-constant writes issued (1 whole block per flush + tail re-pushes)
    u64 state_scissors;               // scissor sets actually issued

} s_render;

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
        gui_log( GUI_LOG_INFO, "using cooked shaders (bin/shaders/gui.{vs,ps}.oshd)" );
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

    // Vertex layout: FLOAT2 pos @0, UNORM16X2 uv @8, UNORM8X4 color @12, HALF2 fx coord @16,
    // UINT fx word @20, UINT tex word @24, stride=28.  Locations 3/4 are the effect band (gui.h):
    // every primitive that is not an SDF surface leaves the word 0, and the fragment tests that
    // first.  Location 5 is the sampling model + bindless slot, which rides the vertex so that a
    // texture change cannot open a draw call.
    //
    // FOUR of the six are packed formats, and the shaders declare all four as plain floats: vertex
    // fetch widens normalized and half attributes on the way in, so the packing is invisible above
    // this line.  It is validated, not assumed -- pipeline_create checks every attribute against
    // the reflected shader input for numeric class and component count, and rejects the pipeline
    // (rather than fetching garbage) if the device cannot read one of these formats at all.
    rhi_vertex_attrib_t attribs[ 6 ] = {
        { .binding = 0, .location = 0, .offset =  0, .format = RHI_VERTEX_FORMAT_FLOAT2     },
        { .binding = 0, .location = 1, .offset =  8, .format = RHI_VERTEX_FORMAT_UNORM16X2  },
        { .binding = 0, .location = 2, .offset = 12, .format = RHI_VERTEX_FORMAT_UNORM8X4   },
        { .binding = 0, .location = 3, .offset = 16, .format = RHI_VERTEX_FORMAT_HALF2      },
        { .binding = 0, .location = 4, .offset = 20, .format = RHI_VERTEX_FORMAT_UINT       },
        { .binding = 0, .location = 5, .offset = 24, .format = RHI_VERTEX_FORMAT_UINT       },
    };

    /* The layout above is spelled in literal offsets, so pin it to the struct it must mirror --
       a field inserted into gui_draw_vert_t would otherwise shift every attribute silently. */
    ORB_STATIC_ASSERT( sizeof( gui_draw_vert_t ) == 28, "gui vertex layout is stated in literal offsets" );
    ORB_STATIC_ASSERT( offsetof( gui_draw_vert_t, uv  ) ==  8, "uv attribute offset drifted" );
    ORB_STATIC_ASSERT( offsetof( gui_draw_vert_t, fxc ) == 16, "fx coord attribute offset drifted" );
    ORB_STATIC_ASSERT( offsetof( gui_draw_vert_t, fx  ) == 20, "fx attribute offset drifted" );
    ORB_STATIC_ASSERT( offsetof( gui_draw_vert_t, tex ) == 24, "tex attribute offset drifted" );

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
        .attribs            = { attribs[ 0 ], attribs[ 1 ], attribs[ 2 ],
                                attribs[ 3 ], attribs[ 4 ], attribs[ 5 ] },
        .attrib_count       = 6,
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

    /* The image sampler: bilinear, and CLAMP on both axes -- a sprite never tiles through the
       sampler (the tessellator repeats quads for that), and REPEAT here would wrap a stretched
       piece onto its neighbour in the atlas.  Non-fatal if it fails: image draws fall back to the
       point sampler below, which looks blocky but renders. */
    s_render.image_sampler = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_LINEAR,
        .mag_filter = RHI_FILTER_LINEAR,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( rhi_handle_valid( s_render.image_sampler ) )
        s_render.image_sampler_idx = rhi()->register_sampler( s_render.image_sampler );

    /* Fonts boot in the DRAW unit now (gui_draw_boot, after the whole server stands up) --
       the shared atlas carries the opaque white texel solid-color draws sample, so solids
       and text still share one texture and merge into one draw. */
    return true;
}

static void
render_shutdown( void )
{
    // Peak draw-list usage over the run, so the caps can be tuned with real numbers.
    gui_log( GUI_LOG_INFO, "peak draw-list usage: verts %u/%u (%.1f%%), idx %u/%u (%.1f%%)%s",
             s_tess_stats.vert_hwm, GUI_MAX_VERTS, 100.0f * s_tess_stats.vert_hwm / (f32)GUI_MAX_VERTS,
             s_tess_stats.idx_hwm,  GUI_MAX_IDX,   100.0f * s_tess_stats.idx_hwm  / (f32)GUI_MAX_IDX,
             s_tess_stats.overflow_ever ? "  -- OVERFLOWED (geometry was dropped)" : "" );

    // Peak draw calls in a single frame -- a measure of batching effectiveness.
    gui_log( GUI_LOG_INFO, "peak draw calls in a frame: %u", cache_draw_call_hwm() );

    /* Per-draw state cost.  The two halves are no longer the same kind of number and are not
       reported as though they were:

         pushes   -- 20-byte tail writes.  Once per FLUSH in normal rendering (the tail is
                     frame-constant since the texture moved into the vertex), plus one per draw in
                     the BATCH view, which is the only thing left that changes it.  Quoted against
                     flushes, so a run that stayed in normal rendering reads ~1.00 and a session
                     spent in the batch view reads far higher -- printing it as a "% suppressed"
                     against draws would just assert 100% forever and measure nothing.
         scissors -- a real redundancy filter with a real hit rate: the clip rect is the only thing
                     that cuts a batch, so consecutive commands often share one, and the suppressed
                     figure is what that saves. */
    if ( s_render.state_draws )
    {
        gui_log( GUI_LOG_INFO,
                 "per-draw state: %llu scissors over %llu draws (%.0f%% suppressed); "
                 "%llu tail pushes over %llu flushes (%.2f per flush)",
                 (unsigned long long)s_render.state_scissors,
                 (unsigned long long)s_render.state_draws,
                 100.0 * ( 1.0 - (f64)s_render.state_scissors / (f64)s_render.state_draws ),
                 (unsigned long long)s_render.state_pushes,
                 (unsigned long long)s_render.state_flushes,
                 s_render.state_flushes ? (f64)s_render.state_pushes / (f64)s_render.state_flushes
                                        : 0.0 );
    }

    /* The last submitted frame may still be executing on the GPU; the destroys below
       (font textures, samplers, pipelines) are immediate, so drain the device first. */
    rhi()->device_wait_idle();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );
    if ( s_render.image_sampler_idx )
        rhi()->unregister_sampler( s_render.image_sampler_idx );
    if ( rhi_handle_valid( s_render.image_sampler ) )
        rhi()->sampler_destroy( s_render.image_sampler );

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

// clang-format on
/*============================================================================================*/
