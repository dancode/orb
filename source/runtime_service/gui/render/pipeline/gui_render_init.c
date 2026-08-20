/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_render_init.c -- Shared GPU resources

    The RENDER phase's one-time setup, shared by every surface for the life of the run: 
    The compiled pipeline (+ wireframe twin), the two bindless samplers (font/coverage + image),
    and the push-constant layout they're built against.  
    
    render_init creates them; render_shutdown tears them down and logs the run's peak pool fills,
    draw-call count and per-draw state cost.

        EMIT    gui_emit_*.c       widgets -> s_draw semantic command list
        BUILD   gui_build_cache.c  diff + tessellate -> s_tess geometry + s_dispatch slot table
        RENDER  this file          shared GPU resources (once) -> submits per surface

    Per-surface state -- a surface's own geometry buffers and the flush that uploads and draws
    them -- lives next door in pipeline/gui_render_submit.c, which reads s_render (this file's static)
    as a shared constant and never writes it.

==============================================================================================*/
#include "engine/sys/sys_host.h"  // sys_exe_dir -- locate the cooked .oshd shaders
                                  // (gui is a static lib: sys is always in the host)

// clang-format off
/*==============================================================================================
    Push constant layout -- must match gui_pc_t in shaders/gui_common.hlsli.

    The cooked container reflects the block's SIZE, and pipeline_create checks this struct
    against it; field ORDER is on the two comment blocks to keep in step.
==============================================================================================*/

typedef struct
{
    f32 mvp[ 16 ];      // column-major ortho matrix       64 bytes
    u32 samp_point;     // bindless sampler: NEAREST        4 bytes
    u32 samp_image;     // bindless sampler: LINEAR         4 bytes
    u32 dbg_flat;       // debug: 1 = flat color (no atlas) 4 bytes
    u32 dbg_tint;       // debug: packed RGBA8 batch tint   4 bytes
    f32 time;           // frame clock, wrapped seconds     4 bytes
    u32 clip_buf;       // bindless slot: frame clip table  4 bytes (0 = no table, no clipping)
    u32 clip_base;      // draw's first clip-table entry    4 bytes
    u32 prim_buf;       // bindless slot: style records     4 bytes (0 = no records bound)
    u32 prim_base;      // the SLOT's first record          4 bytes
    u32 pal_base;       // this frame's palette block       4 bytes
    u32 quad_buf;       // bindless slot: quad-record table 4 bytes
    u32 quad_base;      // flush's region origin            4 bytes (quads, not float4s)
    u32 glyph_buf;      // bindless slot: glyph UV table    4 bytes (0 = no table bound)
                        //   no base: the table is not regioned, so an ID indexes it directly
    u32 tex_cov;        // bindless slot: coverage atlas    4 bytes
    u32 tex_sdf;        // bindless slot: SDF atlas         4 bytes

} gui_push_t;           // total 124 bytes -- well within RHI_MAX_PUSH_CONST_SIZE

/*  The texture and its sampling model USED to live here, one pair per draw call, and that is
    exactly what forced a draw call per texture.  They moved onto the STYLE RECORD each quad names
    (gui.h, gui_prim_t.tex); the fragment reads the slot from there and picks between the two
    SAMPLERS below by the model packed with it.  A glyph quad names no style record at all, so its
    atlas comes from tex_cov / tex_sdf above -- also flush-constant.  What remains in this block is
    per-FRAME state only: in normal rendering nothing in the tail changes across a whole flush, and
    the redundancy filter below pushes it once and then goes quiet.  */

/*  The block is written ONCE per flush, before the dispatch walk, and re-pushed from the tail by
    the two consumers that genuinely vary below that granularity: prim_base, which is per WINDOW
    SLOT because the record arena is packed rather than slabbed, and the BATCH debug view's tint,
    which is deliberately different per draw call (see gui_render_flush).  Both go through the
    tail, and both are filtered against the last value pushed, so a normal frame with one window
    still pushes exactly once.

    Vulkan leaves push constants undefined until written, and the head is written after this
    flush's cmd_bind_pipeline, so a scene pass that bound its own pipeline earlier in the command
    buffer cannot leave a stale matrix behind.

    Derived with offsetof rather than spelled 64/24: the struct is free to be reordered pre-ship
    (CLAUDE.md), and the assert below is what makes that safe -- the split is only valid while the
    mvp is first and the tail is everything after it.  */
#define GUI_PUSH_TAIL_OFF   ( (u32)offsetof( gui_push_t, samp_point ) )
#define GUI_PUSH_TAIL_SIZE  ( (u32)( sizeof( gui_push_t ) - offsetof( gui_push_t, samp_point ) ) )

/*==============================================================================================
    Frame clip table sizing -- the storage buffer clip_coverage reads (gui_fx.hlsli).

    An ENTRY is two vec4s: (x0, y0, x1, y1) snapped pixel edges, then (radius, 0, 0, 0).  A
    REGION is an array of fixed per-window SLABS -- one per stable cache slot, GUI_WIN_CLIP_MAX
    entries each, at cache_idx * GUI_WIN_CLIP_MAX -- the same base the window's quads bake
    into the clip band, so uploads land at fixed offsets and can never overflow.  One region per
    (frame-in-flight, viewport), because the buffer is shared across surfaces whose in-flight
    draws read their own frames' entries.
==============================================================================================*/

#define GUI_CLIP_ENTRY_FLOATS  8u
#define GUI_CLIP_ENTRY_BYTES   ( GUI_CLIP_ENTRY_FLOATS * 4u )
#define GUI_CLIP_REGION_MAX    ( RENDER_MAX_WIN * GUI_WIN_CLIP_MAX )     /* entries per region */
#define GUI_CLIP_REGION_BYTES  ( GUI_CLIP_REGION_MAX * GUI_CLIP_ENTRY_BYTES )
#define GUI_CLIP_REGION_COUNT  ( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS )

/*==============================================================================================
    Primitive record region sizing -- the storage buffer the fragment resolves a shape from.

    Same region scheme as the clip table above and for the same in-flight reason, but the layout
    inside a region differs in the one way that matters: records are PACKED, not slabbed.  A
    window's records sit wherever the arena placed them (win_geo_slot_t.prim_base), so an upload
    lands at a moving offset and the flush pushes that offset per slot -- where a clip slab sits at
    a fixed multiple and needs no per-slot constant at all.
==============================================================================================*/

/* One past the arena, because the DEBUG OVERLAY needs a record of its own and has no window slot
   to get one from.  It builds its quads outside the tessellator entirely, yet it still has to
   name a texture -- the atlas slot, which is only known at flush time -- so "carry style 0 and
   inherit" was never available to it.  One entry at the top of every region, written by
   dbg_flush, is the whole cost. */
#define GUI_PRIM_OVERLAY_ENTRY   GUI_MAX_PRIMS
#define GUI_PRIM_REGION_MAX    ( GUI_MAX_PRIMS + 1u )                    /* records per region */
#define GUI_PRIM_REGION_BYTES  ( GUI_PRIM_REGION_MAX * GUI_PRIM_BYTES )
#define GUI_PRIM_REGION_COUNT  ( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS )

/*  The STYLE PALETTE (gui.h, GUI_PAL_FIRST) sits past every region, in the same buffer.  It is
    not regioned per viewport, because that is what it is FOR: one copy of a record every window
    on every surface can name, where a region's records are reachable only from the window slot
    that wrote them.

    It is regioned per FRAME-IN-FLIGHT and nothing else.  Content is identical across surfaces, so
    a viewport multiplier would buy nothing -- but a style re-bake rewrites the table while earlier
    frames may still be reading it, and one copy per in-flight frame is what makes that write land
    somewhere no draw is looking.  The glyph table answers the same question by replacing its
    buffer wholesale; the palette is small enough that copies are cheaper than a rebuild.

    So the buffer is [ region 0 .. region N-1 | palette 0 .. palette F-1 ], and pc.pal_base names
    this frame's block.  Both bases index in RECORDS, not float4s. */

#define GUI_PAL_REGION_COUNT   RHI_MAX_FRAMES_IN_FLIGHT
#define GUI_PAL_REGION_BYTES   ( GUI_PAL_MAX * GUI_PRIM_BYTES )
#define GUI_PAL_ORIGIN         ( GUI_PRIM_REGION_COUNT * GUI_PRIM_REGION_MAX )  /* in records */

#define GUI_PRIM_BUF_RECORDS   ( GUI_PAL_ORIGIN + GUI_PAL_REGION_COUNT * GUI_PAL_MAX )
#define GUI_PRIM_BUF_BYTES     ( GUI_PRIM_BUF_RECORDS * GUI_PRIM_BYTES )

/*==============================================================================================
    Quad record region sizing -- the geometry table (gui.h, gui_quad_t).

    Quads are packed per window slot exactly as the records above are, in one global buffer with
    the same (frame-in-flight, viewport) region scheme, and the flush pushes the region origin
    (pc.quad_base) once -- a draw's first_vertex carries the arena-absolute quad offset.
==============================================================================================*/

#define GUI_QUAD_REGION_MAX    GUI_MAX_QUADS
#define GUI_QUAD_REGION_BYTES  ( GUI_QUAD_REGION_MAX * GUI_QUAD_BYTES )
#define GUI_QUAD_REGION_COUNT  ( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS )

/*==============================================================================================
    Glyph UV table sizing -- the ID-indexed atlas rects (gui.h, gui_glyph_uv_t).

    NOT regioned, unlike the three tables above.  Those are rewritten every frame, so each
    (frame-in-flight, viewport) needs somewhere of its own to land.  This one is written when its
    generation changes -- a font's pixels entering the atlas, or a repack moving a page -- which is
    a boot-time event and then essentially never, and its contents are identical for every surface.

    So instead of N copies to write into, there is ONE buffer, replaced wholesale on the rare
    rebuild: render_glyph_buf_refresh creates the new one and hands the old to rhi's deferred
    destroy, which already holds it until no frame in flight can be reading it.  Nothing is ever
    overwritten in place, so there is nothing for an in-flight draw to race.
==============================================================================================*/

#define GUI_GLYPH_TABLE_BYTES  ( GUI_GLYPH_TABLE_MAX * (u32)sizeof( gui_glyph_uv_t ) )

ORB_STATIC_ASSERT( offsetof( gui_push_t, mvp ) == 0,
                   "the mvp must lead gui_push_t -- the per-draw push writes everything after it" );
ORB_STATIC_ASSERT( GUI_PUSH_TAIL_OFF == sizeof( ( (gui_push_t*)0 )->mvp ),
                   "gui_push_t tail must start immediately after the mvp" );
ORB_STATIC_ASSERT( GUI_PUSH_TAIL_OFF % 4 == 0 && GUI_PUSH_TAIL_SIZE % 4 == 0,
                   "vkCmdPushConstants requires 4-byte aligned offset and size" );

/*==============================================================================================
    Shared GPU resources -- created once in render_init, destroyed in render_shutdown.

    Immutable across frames and shared by every viewport (and the debug overlay), so never 
    a per-viewport or per-frame bottleneck.
    
    A surface owns NO geometry buffers of its own: the quad, style and clip tables below are one
    buffer each, carved into a region per (frame-in-flight, viewport).  A viewport is a render
    TARGET that windows are dispatched to, not an owner of windows -- the one context emits every
    window and the flush routes each window's geometry to the viewport hosting it.
    
    The viewport list lives in the bound context (core/gui_ctx.c), so this file only ever 
    touches a surface through the GPU pieces passed to it.
==============================================================================================*/

static struct
{
    rhi_pipeline_t  pipeline_quad;      // THE pipeline: bufferless gui_quad shaders + alpha blend
    rhi_pipeline_t  pipeline_quad_wire; // same in VK_POLYGON_MODE_LINE (wireframe debug view)
    rhi_sampler_t   font_sampler;       // coverage sampler: point, U repeats (dash tiling), V clamps
    u32             font_sampler_idx;   // bindless slot for font_sampler

    /* Second sampler, bilinear, for the sampling models that filter: authored sprite art, a
       caller's own textures (a scene render target), and distance-field glyphs.  BOTH slots are
       pushed every flush and the FRAGMENT chooses between them from the sampling model the quad
       resolved, so a draw call binds neither in particular -- which is exactly what lets one batch
       mix a point-sampled glyph atlas with a filtered image.  The model answers on its own:
       coverage is glyphs and icons, which must stay point-sampled to render crisp, and everything
       else is a picture or a field, which must filter. */

    rhi_sampler_t   image_sampler;      // sampler for RGBA images (bilinear clamp)
    u32             image_sampler_idx;  // bindless slot for image_sampler

    /* The frame clip table -- the storage buffer clip_coverage reads (gui_fx.hlsli).  One region
       per (frame-in-flight, viewport): the flush writes each surface's dispatched slots' local
       clip entries into its own region, so no surface's upload can overwrite entries another
       surface's in-flight draws still read.  Registered bindless once; the slot index rides
       pc.clip_buf every flush. */

    rhi_buffer_t    clip_buf;           // storage buffer: all clip regions
    u32             clip_buf_idx;       // bindless buffer slot (never 0 after a successful init)

    /* The style record table (gui.h, gui_prim_t) -- one region per (frame-in-flight,
       viewport), written per window slot by the flush.  Registered bindless once; the slot index
       rides pc.prim_buf and the window's own record base rides pc.prim_base. */

    rhi_buffer_t    prim_buf;           // storage buffer: all record regions
    u32             prim_buf_idx;       // bindless buffer slot (never 0 after a successful init)

    /* The quad-record geometry table (GUI_QUAD_REGION_* above). */

    rhi_buffer_t    quad_buf;           // storage buffer: all quad regions
    u32             quad_buf_idx;       // bindless buffer slot (never 0 after a successful init)

    /* The glyph UV table: ONE buffer, replaced wholesale when its generation changes rather than
       regioned per frame (see the sizing note above).  The slot index rides pc.glyph_buf; there is
       no base to push, since there is only ever one table. */

    rhi_buffer_t    glyph_buf;          // storage buffer: the whole glyph table
    u32             glyph_buf_idx;      // bindless buffer slot (never 0 after a successful init)
    u32             glyph_buf_gen;      // table generation the buffer currently holds

    gui_render_mode_t debug_mode;       // NORMAL / WIREFRAME / BATCH -- how the UI list is rasterized

    /* The frame clock handed down by the orchestrator (gui_render_set_time), already wrapped.
       Held here rather than read from the IO snapshot because s_io lives in the frontend unit and
       this server cannot see it -- the same one-way seam gui_render_set_mode crosses. */

    f32 fx_time;                        // seconds since the first frame, wrapped; -> pc.time

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
    render_load_shaders -- the cooked pair, which is the only way the gui gets a pipeline.

    bin/shaders/gui_quad.{vs,ps}.oshd are cooked from shaders/gui_quad.{vs,ps}.hlsl by the build
    itself (the 'shader' lines on the gui target in orb.targets), so the .hlsl files are the
    single source for what the GPU runs -- there is no embedded SPIR-V array and no GLSL
    transcript to drift against them.  The containers carry reflection, which is what lets
    pipeline_create validate the push range below against the actual SPIR-V.

    All-or-nothing, and a missing file is a hard failure with the path in it: an out-of-date or
    absent cook must say so at init rather than paint a frame from stale bytes.
==============================================================================================*/

static bool
render_load_shaders( rhi_shader_t* out_vert, rhi_shader_t* out_frag )
{
    char dir[ 512 ];
    sys_exe_dir( dir, ( int )sizeof( dir ) );

    char vs_path[ 576 ], ps_path[ 576 ];
    fmt_snprintf( vs_path, sizeof( vs_path ), "%s/shaders/gui_quad.vs.oshd", dir );
    fmt_snprintf( ps_path, sizeof( ps_path ), "%s/shaders/gui_quad.ps.oshd", dir );

    rhi_shader_t vert = rhi()->shader_load_oshd( vs_path, "gui_quad_vert(oshd)" );
    if ( !rhi_handle_valid( vert ) )
    {
        gui_log( GUI_LOG_ERROR, "gui shaders not found -- expected %s (build the gui target to cook them)",
                 vs_path );
        return false;
    }

    rhi_shader_t frag = rhi()->shader_load_oshd( ps_path, "gui_quad_frag(oshd)" );
    if ( !rhi_handle_valid( frag ) )
    {
        gui_log( GUI_LOG_ERROR, "gui shaders not found -- expected %s (build the gui target to cook them)",
                 ps_path );
        rhi()->shader_destroy( vert );
        return false;
    }

    *out_vert = vert;
    *out_frag = frag;
    return true;
}

static void render_shutdown( void );   /* the clip-table failure path below unwinds through it */

/*==============================================================================================
    The glyph UV table's buffer -- created once, then REPLACED (never rewritten) on a rebuild.

    render_glyph_buf_create allocates, registers bindless, and uploads whatever the table holds
    right now.  render_glyph_buf_refresh is the per-flush check: when the table's generation has
    moved, it builds a fresh buffer and retires the old handle and slot, both of which rhi holds
    until no frame in flight can still be reading them (vk_garbage_push / vk_retire_safe_at).

    Replacing rather than overwriting is what makes ONE buffer correct here.  The alternative --
    a copy per frame in flight, like the three per-frame tables -- would be paying 3x for a table
    that is identical every frame, to solve a write hazard that only exists if you write in place.
==============================================================================================*/

static bool
render_glyph_buf_create( void )
{
    s_render.glyph_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = GUI_GLYPH_TABLE_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_glyph_table",
    } );
    if ( !rhi_handle_valid( s_render.glyph_buf ) )
        return false;

    s_render.glyph_buf_idx = rhi()->register_buffer( s_render.glyph_buf );
    if ( s_render.glyph_buf_idx == 0 )
        return false;

    rhi()->buffer_write( s_render.glyph_buf, glyph_table_data(), GUI_GLYPH_TABLE_BYTES, 0 );
    s_render.glyph_buf_gen = glyph_table_generation();
    return true;
}

static void
render_glyph_buf_refresh( void )
{
    u32 gen = glyph_table_generation();
    if ( s_render.glyph_buf_gen == gen )
        return;

    rhi_buffer_t old_buf = s_render.glyph_buf;
    u32          old_idx = s_render.glyph_buf_idx;

    if ( !render_glyph_buf_create() )
    {
        /* Keep drawing from the buffer we have: its rects are one generation stale, which is a
           wrong glyph rectangle, where a zeroed bindless slot is every character gone. */
        s_render.glyph_buf     = old_buf;
        s_render.glyph_buf_idx = old_idx;
        GUI_WARN_ONCE( "glyph table rebuild could not allocate -- holding generation %u\n",
                       s_render.glyph_buf_gen );
        return;
    }

    if ( old_idx )
        rhi()->unregister_buffer( old_idx );
    if ( rhi_handle_valid( old_buf ) )
        rhi()->buffer_destroy( old_buf );
}

static bool
render_init( void )
{
    rhi_shader_t vert = { RHI_NULL_HANDLE };
    rhi_shader_t frag = { RHI_NULL_HANDLE };

    if ( !render_load_shaders( &vert, &frag ) )
        return false;

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

    /* BUFFERLESS: no attributes and vertex_stride 0, so the RHI creates the pipeline with no
       vertex binding at all -- the stage pulls everything from the quad table by SV_VertexID.
       One descriptor shared by both pipelines; only polygon_mode differs.  The wireframe variant
       (VK_POLYGON_MODE_LINE) lets the debug render mode draw triangle edges through the same
       shaders and push range -- the flush just binds whichever the mode selects. */
    rhi_pipeline_desc_t pdesc = {
        .vert               = vert,
        .frag               = frag,
        .attrib_count       = 0,
        .vertex_stride      = 0,
        .cull               = RHI_CULL_NONE,
        .polygon_mode       = RHI_POLYGON_FILL,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( gui_push_t ),
        .debug_name         = "gui_quad",
    };
    s_render.pipeline_quad = rhi()->pipeline_create( &pdesc );

    /* Wireframe pipeline: the debug-view (normal / wireframe / batch-tint) toggle's only extra
       GPU resource.  gui_render_flush falls back to the fill pipeline if this one ever fails to
       create. */
    pdesc.polygon_mode = RHI_POLYGON_LINE;
    pdesc.debug_name   = "gui_quad_wire";
    s_render.pipeline_quad_wire = rhi()->pipeline_create( &pdesc );

    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );

    /* The wireframe pipeline is a debug convenience -- a failure there is non-fatal (the mode just
       falls back to the fill pipeline at flush time); only the fill pipeline is required. */
    if ( !rhi_handle_valid( s_render.pipeline_quad ) )
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
        if ( rhi_handle_valid( s_render.pipeline_quad_wire ) )
            rhi()->pipeline_destroy( s_render.pipeline_quad_wire );
        rhi()->pipeline_destroy( s_render.pipeline_quad );
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

    /* The frame clip table.  REQUIRED, not a nicety: the fragment is the only thing cutting
       content to its clip now (the scissor is a full-surface constant), so running without the
       table would let every scrolled row paint past its region. */
    s_render.clip_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = GUI_CLIP_REGION_COUNT * GUI_CLIP_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_clip_table",
    } );
    if ( rhi_handle_valid( s_render.clip_buf ) )
        s_render.clip_buf_idx = rhi()->register_buffer( s_render.clip_buf );
    if ( s_render.clip_buf_idx == 0 )
    {
        gui_log( GUI_LOG_ERROR, "clip table buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* The primitive record table.  REQUIRED for the same reason the clip table is: once the
       fragment resolves its shape from a record, a frame without one has nothing to draw.  Holds
       the per-(frame, viewport) arena regions AND the palette blocks behind them. */
    s_render.prim_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = GUI_PRIM_BUF_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_prim_table",
    } );
    if ( rhi_handle_valid( s_render.prim_buf ) )
        s_render.prim_buf_idx = rhi()->register_buffer( s_render.prim_buf );
    if ( s_render.prim_buf_idx == 0 )
    {
        gui_log( GUI_LOG_ERROR, "primitive record buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* The quad-record geometry table.  REQUIRED like the two tables above: every draw pulls its
       placement from it. */
    s_render.quad_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = GUI_QUAD_REGION_COUNT * GUI_QUAD_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_quad_table",
    } );
    if ( rhi_handle_valid( s_render.quad_buf ) )
        s_render.quad_buf_idx = rhi()->register_buffer( s_render.quad_buf );
    if ( s_render.quad_buf_idx == 0 )
    {
        gui_log( GUI_LOG_ERROR, "quad record buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* The glyph UV table.  REQUIRED: a glyph quad carries a table ID in place of its atlas rect,
       so without the table every character samples texel zero. */
    if ( !render_glyph_buf_create() )
    {
        gui_log( GUI_LOG_ERROR, "glyph table buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* Fonts boot in the DRAW unit now (gui_draw_boot, after the whole server stands up) --
       the shared atlas carries the opaque white texel solid-color draws sample, so solids
       and text still share one texture and merge into one draw. */
    return true;
}

static void
render_shutdown( void )
{
    /* Peak fill of each BUILD pool over the run, so the caps can be moved from measurement rather
       than from a guess.  The three are independent budgets: quads scale with how much is on
       screen, styles with how many distinct looks it uses, and gpu commands with how the viewport
       splits them. */
    gui_log( GUI_LOG_INFO,
             "peak build pools: quads %u/%u (%.1f%%), styles %u/%u (%.1f%%), gpu cmds %u/%u (%.1f%%)",
             s_tess_stats.quad_hwm, GUI_MAX_QUADS, 100.0f * s_tess_stats.quad_hwm / (f32)GUI_MAX_QUADS,
             s_tess_stats.prim_hwm, GUI_MAX_PRIMS, 100.0f * s_tess_stats.prim_hwm / (f32)GUI_MAX_PRIMS,
             s_tess_stats.cmd_hwm,  GUI_MAX_CMDS,  100.0f * s_tess_stats.cmd_hwm  / (f32)GUI_MAX_CMDS );

    if ( s_tess_stats.overflow_walls )
    {
        char walls[ 128 ];
        gui_log( GUI_LOG_WARN, "  -- OVERFLOWED this run (content dropped): %s",
                 tess_overflow_walls( s_tess_stats.overflow_walls, walls, (u32)sizeof( walls ) ) );
    }
    if ( s_tess_stats.uncacheable_wins )
        gui_log( GUI_LOG_WARN, "  -- %u window placement%s exceeded WIN_SLOT_CMD_MAX (%u) and "
                               "re-tessellated every frame",
                 s_tess_stats.uncacheable_wins, s_tess_stats.uncacheable_wins == 1u ? "" : "s",
                 (unsigned)WIN_SLOT_CMD_MAX );

    // Peak draw calls in a single frame -- a measure of batching effectiveness.
    gui_log( GUI_LOG_INFO, "peak draw calls in a frame: %u", cache_draw_call_hwm() );

    /* Per-draw state cost.
         pushes   -- tail writes: the full block once per flush, plus one tail per dispatched
                     window slot (prim_base, the only per-slot constant), plus one per draw in the
                     BATCH debug view.  Quoted against flushes, so a normal run reads roughly
                     1 + the number of windows on the surface.
         scissors -- exactly one full-surface set per flush now: clipping is per-fragment (the
                     clip band), so scissor state never changes mid-walk. */
    if ( s_render.state_draws )
    {
        gui_log( GUI_LOG_INFO,
                 "per-draw state: %llu scissors, %llu draws over %llu flushes; "
                 "%llu tail pushes (%.2f per flush)",
                 (unsigned long long)s_render.state_scissors,
                 (unsigned long long)s_render.state_draws,
                 (unsigned long long)s_render.state_flushes,
                 (unsigned long long)s_render.state_pushes,
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

    if ( s_render.clip_buf_idx )
        rhi()->unregister_buffer( s_render.clip_buf_idx );
    if ( rhi_handle_valid( s_render.clip_buf ) )
        rhi()->buffer_destroy( s_render.clip_buf );

    if ( s_render.prim_buf_idx )
        rhi()->unregister_buffer( s_render.prim_buf_idx );
    if ( rhi_handle_valid( s_render.prim_buf ) )
        rhi()->buffer_destroy( s_render.prim_buf );

    if ( s_render.quad_buf_idx )
        rhi()->unregister_buffer( s_render.quad_buf_idx );
    if ( rhi_handle_valid( s_render.quad_buf ) )
        rhi()->buffer_destroy( s_render.quad_buf );

    if ( s_render.glyph_buf_idx )
        rhi()->unregister_buffer( s_render.glyph_buf_idx );
    if ( rhi_handle_valid( s_render.glyph_buf ) )
        rhi()->buffer_destroy( s_render.glyph_buf );

    if ( rhi_handle_valid( s_render.pipeline_quad_wire ) )
        rhi()->pipeline_destroy( s_render.pipeline_quad_wire );
    if ( rhi_handle_valid( s_render.pipeline_quad ) )
        rhi()->pipeline_destroy( s_render.pipeline_quad );

    memset( &s_render, 0, sizeof( s_render ) );
}

/* Memory accounting lives in render/gui_render_mem.c (backend_memory) -- the LAST include
   of the unity TU, so it can sizeof every backend static, including the capture/debug files
   included after this one. */

// clang-format on
/*============================================================================================*/
