/*==============================================================================================

    runtime_service/imgui/imgui_render.c -- GPU resource management and draw-list flush.

    imgui_render_init:
        Creates the shared pipeline + font sampler; viewport_create makes a surface's VB/IB.

    imgui_render_flush:
        Tessellates the frame's semantic command list (via tess_dispatch in imgui_render_tess.c,
        included just before this file), uploads the resulting vertex/index data into the
        viewport's frame-region buffers, opens a LOAD render pass on the target, and emits
        one indexed draw call per GPU command.  imgui_render_shutdown destroys all GPU objects.

    Included by imgui_backend.c after imgui_render_tess.c (which defines s_tess, tess_reset,
    tess_dispatch and all tessellation helpers) and imgui_draw.c (s_draw).

==============================================================================================*/
#include "runtime_service/imgui/imgui_internal.h"   /* imgui_viewport_t, imgui_context_t, IMGUI_MAX_VIEWPORTS */
// clang-format off

/*==============================================================================================
    Push constant layout (72 bytes; must match imgui_shader.h GLSL source)
==============================================================================================*/

typedef struct
{
    f32 mvp[ 16 ];      /* column-major ortho matrix    64 bytes */
    u32 tex_idx;        /* bindless texture slot         4 bytes */
    u32 samp_idx;       /* bindless sampler slot         4 bytes */
    u32 dbg_flat;       /* debug: 1 = flat color (no atlas) 4 bytes */
    u32 dbg_tint;       /* debug: packed RGBA8 batch tint   4 bytes */

} imgui_push_t;         /* total 80 bytes -- well within RHI_MAX_PUSH_CONST_SIZE */

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

/* imgui_viewport_t (the per-viewport surface record) and IMGUI_MAX_VIEWPORTS are defined in
   imgui_internal.h, along with the struct imgui_dock_node_t forward declaration. */

/* The viewport list lives in the bound context (imgui_context_t, imgui_ctx.c): viewports[0] is the
   main swapchain, created at init.  render.c only ever touches a viewport through a passed pointer
   (viewport_create / viewport_destroy / imgui_render_flush), so it needs no instance of its own. */

/* Shared GPU resources: created once in imgui_render_init(), destroyed in imgui_render_shutdown().
   Immutable across frames and shared by every viewport (and the debug overlay), so never a per-
   viewport or per-frame bottleneck. */

static struct
{
    rhi_pipeline_t  pipeline;           // compiled pipeline with imgui shaders + vertex layout + alpha blend
    rhi_pipeline_t  pipeline_wire;      // identical pipeline rasterized in VK_POLYGON_MODE_LINE (wireframe view)
    rhi_sampler_t   font_sampler;       // sampler for font textures (point clamp)
    u32             font_sampler_idx;   // bindless slot for font_sampler

    imgui_render_mode_t debug_mode;     // NORMAL / WIREFRAME / BATCH -- how the UI list is rasterized

    u32             draw_call_hwm;      // peak indexed draw calls in a single frame (global stat)

    /* Per-frame render stats.  accum builds across this frame's flush(es) -- one per surface;
       stats_pub is the last frame's completed totals, promoted from accum at frame_begin.  The
       overlay reads stats_pub during the build, one frame behind the geometry it describes. */
    imgui_render_stats_t accum;
    imgui_render_stats_t stats_pub;

} s_render;

/*----------------------------------------------------------------------------------------------
    Once-per-frame tessellation cache.

    The semantic command list (s_draw) is shared by every surface, so the geometry it tessellates
    into -- and the z-ordered run table replayed from it -- is byte-identical on every surface's
    flush.  render_build_frame does that work lazily on the first flush of a frame and stamps
    s_frame_built; subsequent surfaces that frame reuse s_tess + s_runs and only re-do the cheap
    per-surface part (upload, scissor, draw partition).  imgui_render_frame_reset clears the guard
    at frame_begin.  Building lazily (rather than at frame_begin) means a frame that flushes no
    surface does no tessellation at all.
----------------------------------------------------------------------------------------------*/

typedef struct { u32 start, count, first_index, z; } draw_run_t;

static draw_run_t s_runs[ IMGUI_MAX_CMDS ];   /* z-grouped command runs, sorted back-to-front */
static u32        s_run_count;                /* runs valid this frame                         */
static bool       s_frame_built;              /* tessellation + runs computed this frame        */

/* Dispatch order fed to tess_dispatch: a permutation of the semantic command list that groups
   commands by clip rect within each z-run, so equal-clip shapes tessellate contiguously and merge
   into one GPU batch (the scissor change is what opens a batch).  Built fresh each frame by
   render_build_order; see the note there for the ordering-safety argument. */
static u32 s_order[ IMGUI_MAX_CMDS ];

/* Per-surface geometry span within the shared vertex/index lists -- the half-open [lo,hi) range of
   vertex slots and index slots touched by the commands tagged for each viewport.  Computed once in
   render_build_frame; each surface's flush uploads only its own span (written at its real offset, so
   the absolute indices still resolve) instead of the whole shared buffer.  For a single surface the
   span is the entire buffer, so the common case is unchanged. */
static u32 s_vp_vtx_lo[ IMGUI_MAX_VIEWPORTS ];
static u32 s_vp_vtx_hi[ IMGUI_MAX_VIEWPORTS ];
static u32 s_vp_idx_lo[ IMGUI_MAX_VIEWPORTS ];
static u32 s_vp_idx_hi[ IMGUI_MAX_VIEWPORTS ];

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
    viewport_create / viewport_destroy -- a surface's own GPU geometry buffers.

    Per viewport so each surface has an independent vb/ib ring (one region per frame-in-flight).
    Called once per viewport: the main swapchain at init, a torn-off floater on tear-off.  The
    shared pipeline / sampler / atlas are NOT here -- those are created once in imgui_render_init.
----------------------------------------------------------------------------------------------*/

bool
viewport_create( imgui_viewport_t* vp, rhi_texture_t target, i32 win_id )
{
    vp->target    = target;
    vp->win_id    = win_id;     /* OS window hosting this surface; -1 = unassociated */
    vp->rhi_ctx   = RHI_CTX_INVALID;  /* set only by viewport_spawn for an imgui-owned floater */
    vp->owned     = false;      /* host-provided unless viewport_spawn flips it */
    vp->pending_close = false;  /* owned floater close request; serviced by viewport_update */
    vp->disp_w    = 0;          /* drawable size set by the host before build; 0 = fall back to main */
    vp->disp_h    = 0;
    vp->caption_inset = 0.0f;   /* no native shell band until one publishes it during the build */
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

void
viewport_destroy( imgui_viewport_t* vp )
{
    /* Owned floater: destroy the rhi context FIRST.  context_destroy idles the GPU (waits the
       device) before tearing down its swapchain/sync, so the geometry-buffer frees below are then
       safe -- matching the host's own shutdown order (drain, free buffers, close window).  A
       host-provided surface (owned == false) leaves its context to the host; imgui frees only the
       GPU buffers it created via viewport_create. */
    if ( vp->owned && vp->rhi_ctx != RHI_CTX_INVALID )
        rhi()->context_destroy( vp->rhi_ctx );

    if ( rhi_handle_valid( vp->ib ) ) rhi()->buffer_destroy( vp->ib );
    if ( rhi_handle_valid( vp->vb ) ) rhi()->buffer_destroy( vp->vb );

    /* Owned floater: close the OS window imgui opened.  Only after the context (and thus the
       swapchain bound to this window's surface) is gone. */
    if ( vp->owned && vp->win_id >= 0 )
        app()->window_close( vp->win_id );

    vp->vb            = ( rhi_buffer_t ){ 0 };
    vp->ib            = ( rhi_buffer_t ){ 0 };
    vp->win_id        = -1;            /* slot freed -> no window matches it for input routing */
    vp->rhi_ctx       = RHI_CTX_INVALID;
    vp->owned         = false;
    vp->pending_close = false;
}

/*----------------------------------------------------------------------------------------------
    imgui_render_init -- create the shared GPU resources (pipeline, font sampler, atlas).
    Per-viewport geometry buffers are created separately by viewport_create.
----------------------------------------------------------------------------------------------*/

bool
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

    /* One descriptor shared by both pipelines; only polygon_mode differs.  The wireframe variant
       (VK_POLYGON_MODE_LINE) lets the debug render mode draw triangle edges through the same
       shaders, vertex layout, and push range -- the flush just binds whichever the mode selects. */
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

    /* Font sampler: nearest filter.  V/W clamp-to-edge (no bleeding between atlas glyph rows);
       U repeats so a dashed line's single quad can tile an atlas stipple row along its length.
       Glyph and white-texel U coords stay within [0,1], so wrapping never affects text or fills. */
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

imgui_mem_stats_t
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

/*----------------------------------------------------------------------------------------------
    imgui_render_stats / imgui_render_stats_publish -- per-frame geometry + batch counts.

    The flush accumulates into s_render.accum; publish promotes it to s_render.stats_pub and clears
    the accumulator, so the value read during a build is the previous frame's completed totals.
----------------------------------------------------------------------------------------------*/

imgui_render_stats_t
imgui_render_stats( void )
{
    return s_render.stats_pub;
}

void
imgui_render_stats_publish( void )
{
    s_render.stats_pub = s_render.accum;
    s_render.accum     = ( imgui_render_stats_t ){ 0 };
}

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

/*----------------------------------------------------------------------------------------------
    imgui_render_shutdown -- destroy all GPU resources.  Call before rhi()->shutdown().
----------------------------------------------------------------------------------------------*/

void
imgui_render_shutdown( void )
{
    /* Peak draw-list usage over the run, so the caps can be tuned with real numbers. */
    printf( "[imgui] peak draw-list usage: verts %u/%u (%.1f%%), idx %u/%u (%.1f%%)%s\n",
            s_tess.vert_hwm, IMGUI_MAX_VERTS, 100.0f * s_tess.vert_hwm / (f32)IMGUI_MAX_VERTS,
            s_tess.idx_hwm,  IMGUI_MAX_IDX,   100.0f * s_tess.idx_hwm  / (f32)IMGUI_MAX_IDX,
            s_tess.overflow_ever ? "  -- OVERFLOWED (geometry was dropped)" : "" );

    /* Peak draw calls in a single frame -- a measure of batching effectiveness. */
    printf( "[imgui] peak draw calls in a frame: %u\n", s_render.draw_call_hwm );

    font_shutdown();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );

    if ( rhi_handle_valid( s_render.pipeline_wire ) )
        rhi()->pipeline_destroy( s_render.pipeline_wire );
    if ( rhi_handle_valid( s_render.pipeline ) )
        rhi()->pipeline_destroy( s_render.pipeline );

    /* Per-viewport geometry buffers are released by viewport_destroy (driven from imgui_shutdown). */

    memset( &s_render, 0, sizeof( s_render ) );
}

/*----------------------------------------------------------------------------------------------
    imgui_render_set_mode / imgui_render_get_mode -- the debug render view (normal / wireframe /
    batch-tint).  Cheap to flip every frame; read by imgui_render_flush below.
----------------------------------------------------------------------------------------------*/

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

/*----------------------------------------------------------------------------------------------
    batch_debug_color -- a distinct, saturated, fully-opaque color per draw-call index for the
    BATCH view.  Packed RGBA8 (R low byte), matching the shader's dbg_tint decode and IMGUI_COLOR
    byte order.  A 12-entry table cycles; consecutive entries are spread around the hue wheel so
    neighbouring batches stay easy to tell apart, and the count wrapping is harmless (it only has
    to make boundaries visible, not encode an exact id).
----------------------------------------------------------------------------------------------*/

static u32
batch_debug_color( u32 i )
{
    static const u32 palette[ 12 ] = {
        IMGUI_COLOR( 0xE6, 0x39, 0x46, 0xFF ),   /* red      */
        IMGUI_COLOR( 0x2A, 0x9D, 0x8F, 0xFF ),   /* teal     */
        IMGUI_COLOR( 0xE9, 0xC4, 0x6A, 0xFF ),   /* yellow   */
        IMGUI_COLOR( 0x45, 0x7B, 0x9D, 0xFF ),   /* blue     */
        IMGUI_COLOR( 0xF4, 0x7A, 0x20, 0xFF ),   /* orange   */
        IMGUI_COLOR( 0x8E, 0x44, 0xAD, 0xFF ),   /* purple   */
        IMGUI_COLOR( 0x6A, 0xBE, 0x30, 0xFF ),   /* green    */
        IMGUI_COLOR( 0xD6, 0x4B, 0x9C, 0xFF ),   /* pink     */
        IMGUI_COLOR( 0x34, 0x98, 0xDB, 0xFF ),   /* sky      */
        IMGUI_COLOR( 0xC0, 0x8A, 0x3E, 0xFF ),   /* brown    */
        IMGUI_COLOR( 0x1A, 0xBC, 0x9C, 0xFF ),   /* mint     */
        IMGUI_COLOR( 0xBD, 0xC3, 0xC7, 0xFF ),   /* silver   */
    };
    return palette[ i % 12u ];
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

/* imgui_render_frame_reset -- clear the once-per-frame tessellation guard (see s_frame_built).
   Called by imgui_frame_begin right after draw_reset, before the build emits any commands. */
void
imgui_render_frame_reset( void )
{
    s_frame_built = false;
}

/* Exact clip equality -- the same field-by-field compare tess_ensure_gpu_cmd uses to decide a
   batch boundary, so grouping here lines up exactly with how batches are opened at tessellation. */
static bool
clip_eq( imgui_rect_t a, imgui_rect_t b )
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/*----------------------------------------------------------------------------------------------
    render_build_order -- compute the clip-grouped dispatch order written to s_order.

    Walks the semantic command list and, within each contiguous z-run (immediate mode emits one
    window completely before the next, so a run is a maximal same-z span), emits indices grouped by
    first-seen clip rect.  Equal-clip shapes thus tessellate back-to-back and collapse into a single
    GPU batch instead of re-splitting every time a child's scissor pushes and pops -- the window
    chrome, body, and scrollbars reunify into one batch and each child becomes one more.

    Ordering safety: this only reorders commands that share a z (one window) but differ in clip.
    Within a clip group the original emit order is preserved (the inner loop is stable), so
    overlapping shapes under one scissor still paint in declaration order.  Across clips inside a
    window the regions nest -- the window clip strictly contains each child clip, sibling children
    are disjoint -- so two shapes in different groups only share pixels in a nested overlap, where
    the parent must paint under the child; first-seen order keeps the enclosing window's commands
    (emitted first) in the earlier group, so they still draw under the child.  Window chrome that
    must overpaint scrolled body content shares the window clip with that content (own_clip false),
    so it stays in the same group and keeps its emit order after it.

    Cost: an index permutation only -- no geometry or struct is copied; tess_dispatch builds the
    vertices once, in this order.  O(run_len * distinct_clips); distinct_clips per window is a
    handful.  A run with more distinct clips than groups[] holds is left in natural emit order
    (still correct, just unmerged) rather than capping the UI.
----------------------------------------------------------------------------------------------*/

#define RENDER_MAX_CLIP_GROUPS 64

static u32
render_build_order( const imgui_cmd_t* cmds, u32 count )
{
    u32 n = 0;
    for ( u32 rs = 0; rs < count; )
    {
        /* Maximal same-z span [rs, re) -- one window's commands. */
        u32 z  = cmds[ rs ].z;
        u32 re = rs;
        while ( re < count && cmds[ re ].z == z )
            ++re;

        /* Distinct clips in first-seen order. */
        imgui_rect_t groups[ RENDER_MAX_CLIP_GROUPS ];
        u32          ng       = 0;
        bool         overflow = false;
        for ( u32 i = rs; i < re && !overflow; ++i )
        {
            bool seen = false;
            for ( u32 g = 0; g < ng; ++g )
                if ( clip_eq( groups[ g ], cmds[ i ].clip ) ) { seen = true; break; }
            if ( !seen )
            {
                if ( ng >= RENDER_MAX_CLIP_GROUPS ) { overflow = true; break; }
                groups[ ng++ ] = cmds[ i ].clip;
            }
        }

        if ( overflow )
        {
            /* Too many distinct clips to group: keep this run's natural emit order. */
            for ( u32 i = rs; i < re; ++i )
                s_order[ n++ ] = i;
        }
        else
        {
            /* Emit each clip group in first-seen order; stable within a group. */
            for ( u32 g = 0; g < ng; ++g )
                for ( u32 i = rs; i < re; ++i )
                    if ( clip_eq( groups[ g ], cmds[ i ].clip ) )
                        s_order[ n++ ] = i;
        }

        rs = re;
    }
    return n;   /* == count */
}

/*----------------------------------------------------------------------------------------------
    render_build_frame -- tessellate the shared command list and build the sorted run table.

    Runs once per frame (guarded by s_frame_built): the first surface flush triggers it, every
    other surface that frame reuses s_tess + s_runs.  Produces the geometry (s_tess.verts/indices),
    the per-run absolute first_index, and the back-to-front z order -- all surface-independent.
    Geometry stats and the peak/overflow bookkeeping live here so they are counted once, not once
    per surface.
----------------------------------------------------------------------------------------------*/

static void
render_build_frame( void )
{
    if ( s_frame_built )
        return;
    s_frame_built = true;

    /* Tessellate semantic commands into vertex/index geometry (s_tess).  Dispatch in clip-grouped
       order (within each z-run) so equal-clip shapes batch together instead of re-splitting on every
       child scissor; render_build_order returns a permutation, not a copy of the geometry. */
    tess_reset();
    u32 order_count = render_build_order( s_draw.cmds, s_draw.cmd_count );
    tess_dispatch( s_draw.cmds, s_order, order_count );

    /* Track peak tessellated geometry.  Warn once on the first overflow. */
    if ( s_tess.vert_count > s_tess.vert_hwm ) s_tess.vert_hwm = s_tess.vert_count;
    if ( s_tess.idx_count  > s_tess.idx_hwm  ) s_tess.idx_hwm  = s_tess.idx_count;

    if ( s_tess.overflow && !s_tess.overflow_ever )
    {
        printf( "[imgui] WARNING: draw list overflow -- geometry dropped this frame "
                "(verts capped at %u, idx capped at %u). Raise IMGUI_MAX_VERTS / IMGUI_MAX_IDX.\n",
                IMGUI_MAX_VERTS, IMGUI_MAX_IDX );
    }
    if ( s_tess.overflow )
        s_tess.overflow_ever = true;

    /* Group commands into runs of one sort key, then order runs back-to-front by z.

       Commands sharing a sort key are contiguous in the list (immediate mode emits one window
       completely before the next), so a single forward pass collects the runs and the absolute
       first_index at each run's start.  The runs are then stable-sorted by z -- vertices and
       indices never move, only the order in which command ranges are replayed -- so a higher-z
       (raised) window paints last and therefore on top, with equal-z runs keeping their order.
       Surface-independent, so it is built once and replayed by every surface's flush. */
    s_run_count = 0;
    for ( u32 i = 0, first_index = 0; i < s_tess.cmd_count; )
    {
        u32 z      = s_tess.cmd_z[ i ];
        u32 start  = i;
        u32 run_fi = first_index;
        while ( i < s_tess.cmd_count && s_tess.cmd_z[ i ] == z )
            first_index += s_tess.cmds[ i++ ].elem_count;

        s_runs[ s_run_count++ ] = ( draw_run_t ){ start, i - start, run_fi, z };
    }

    /* Stable insertion sort by z (ascending = back-to-front; run_count is small). */
    for ( u32 a = 1; a < s_run_count; ++a )
    {
        draw_run_t key = s_runs[ a ];
        u32        b   = a;
        while ( b > 0 && s_runs[ b - 1 ].z > key.z )
        {
            s_runs[ b ] = s_runs[ b - 1 ];
            --b;
        }
        s_runs[ b ] = key;
    }

    /* Per-surface geometry span: the [lo,hi) vertex + index range each viewport's commands touch,
       so its flush uploads only that slice rather than the whole shared buffer.  Walk the command
       list in emit order (where the index buffer is laid out and cmd_vbase is monotonic) and widen
       each command's viewport range; a command's vertex span is [cmd_vbase[i], cmd_vbase[i+1]) and
       its index span is [ibase, ibase+elem_count).  A viewport with no commands keeps lo>hi (empty,
       so its flush uploads nothing). */
    for ( u32 v = 0; v < IMGUI_MAX_VIEWPORTS; ++v )
    {
        s_vp_vtx_lo[ v ] = s_tess.vert_count;  s_vp_vtx_hi[ v ] = 0;
        s_vp_idx_lo[ v ] = s_tess.idx_count;   s_vp_idx_hi[ v ] = 0;
    }
    for ( u32 i = 0, ibase = 0; i < s_tess.cmd_count; ++i )
    {
        u32 vp_i = s_tess.cmd_vp[ i ];
        u32 vlo  = s_tess.cmd_vbase[ i ];
        u32 vhi  = ( i + 1 < s_tess.cmd_count ) ? s_tess.cmd_vbase[ i + 1 ] : s_tess.vert_count;
        u32 ic   = s_tess.cmds[ i ].elem_count;
        if ( vp_i < IMGUI_MAX_VIEWPORTS )
        {
            if ( vlo         < s_vp_vtx_lo[ vp_i ] ) s_vp_vtx_lo[ vp_i ] = vlo;
            if ( vhi         > s_vp_vtx_hi[ vp_i ] ) s_vp_vtx_hi[ vp_i ] = vhi;
            if ( ibase       < s_vp_idx_lo[ vp_i ] ) s_vp_idx_lo[ vp_i ] = ibase;
            if ( ibase + ic  > s_vp_idx_hi[ vp_i ] ) s_vp_idx_hi[ vp_i ] = ibase + ic;
        }
        ibase += ic;
    }

    /* Per-frame emitted geometry stats -- the tessellated list is the whole shared list, so these
       are the same for every surface; record them once here.  draw_calls are this surface's own
       partition and are summed across surfaces in the flush below. */
    s_render.accum.cmd_count  = s_draw.cmd_count;
    s_render.accum.vert_count = s_tess.vert_count;
    s_render.accum.tri_count  = s_tess.idx_count / 3u;

    /* Per-frame emitted geometry -- the verts/indices this frame actually pushed into the draw
       list (peaks vs. the caps shown for context).  Watch these move as UI state changes; printed
       only when the counts differ from last frame to filter the spam.  Once per frame now. */
    static u32 prev_verts = ~0u, prev_idx = ~0u;   /* sentinel: forces a print on the first frame */
    if ( s_render_debug_geometry && ( s_tess.vert_count != prev_verts || s_tess.idx_count != prev_idx ) )
    {
        printf( "[imgui] geometry this frame: verts %u/%u (peak %u), idx %u/%u (peak %u)\n",
                s_tess.vert_count, IMGUI_MAX_VERTS, s_tess.vert_hwm,
                s_tess.idx_count,  IMGUI_MAX_IDX,   s_tess.idx_hwm );
        prev_verts = s_tess.vert_count;
        prev_idx   = s_tess.idx_count;
    }
}

void
imgui_render_flush( imgui_viewport_t* vp, u32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( s_draw.cmd_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    /* Tessellate + sort the shared list once per frame; this surface reuses the cached result. */
    render_build_frame();

    /* Select this frame's geometry region so the upload cannot clobber data the GPU
       is still reading for another in-flight frame. */
    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = frame * (u32)IMGUI_VB_REGION_BYTES;
    u32 ib_off = frame * (u32)IMGUI_IB_REGION_BYTES;

    /* Upload only the slice of the shared geometry this surface draws (its [lo,hi) span from
       render_build_frame), written at its real offset so the absolute indices still resolve.  The
       untouched parts of the region keep stale data the surface never indexes.  For a single surface
       the span covers the whole buffer, so this is identical to a full upload in the common case. */
    u32 vtx_lo = s_vp_vtx_lo[ vp_index ], vtx_hi = s_vp_vtx_hi[ vp_index ];
    u32 idx_lo = s_vp_idx_lo[ vp_index ], idx_hi = s_vp_idx_hi[ vp_index ];

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

    /* Open a LOAD pass on the swapchain color target (no depth needed).
       LOAD preserves the scene content rendered before this call; CLEAR would wipe it. */
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

    /* Wireframe mode binds the LINE pipeline (falling back to fill if it failed to compile);
       normal + batch modes both rasterize filled triangles. */
    rhi_pipeline_t pipe = ( s_render.debug_mode == IMGUI_RENDER_WIREFRAME
                            && rhi_handle_valid( s_render.pipeline_wire ) )
                        ? s_render.pipeline_wire : s_render.pipeline;
    rhi()->cmd_bind_pipeline( cmd, pipe );
    rhi()->cmd_bind_bindless( cmd );
    /* Bind this frame's region; index values and first_index stay region-relative. */
    rhi()->cmd_bind_vertex_buffer( cmd, vp->vb, vb_off );
    rhi()->cmd_bind_index_buffer( cmd, vp->ib, ib_off, RHI_INDEX_TYPE_UINT16 );

    /* Ortho matrix: pixel [0,w]x[0,h] -> NDC [-1,+1]x[-1,+1]. */
    imgui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );

    /* The z-grouped, back-to-front run table (s_runs) was built once this frame by
       render_build_frame; this surface just replays it, drawing only its own partition. */
    push.samp_idx = s_render.font_sampler_idx;

    /* Debug render mode push state.  dbg_flat makes the fragment bypass the atlas and emit a flat
       color: WIREFRAME keeps each window's vertex color (tint 0), BATCH overrides it per draw call
       with a distinct palette color below.  NORMAL leaves both 0 for the textured/blended path. */
    bool batch_view = ( s_render.debug_mode == IMGUI_RENDER_BATCH );
    push.dbg_flat   = ( s_render.debug_mode == IMGUI_RENDER_NORMAL ) ? 0u : 1u;
    push.dbg_tint   = 0u;

    u32 draw_calls = 0;   /* indexed draws actually emitted this frame (one per non-empty command) */

    for ( u32 r = 0; r < s_run_count; ++r )
    {
        u32 first_index = s_runs[ r ].first_index;
        for ( u32 k = 0; k < s_runs[ r ].count; ++k )
        {
            const imgui_gpu_cmd_t* dc = &s_tess.cmds[ s_runs[ r ].start + k ];
            if ( dc->elem_count == 0 )
                continue;

            /* Partition: a command bound for another surface is not drawn here, but its index
               range is still stepped over so the commands kept on this surface address the right
               slice of the (shared, absolute) index buffer. */
            if ( s_tess.cmd_vp[ s_runs[ r ].start + k ] != vp_index )
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
            /* Batch view: tint this draw call by its running index, so each batch reads as a
               distinct solid color and a color change in the image marks a batch boundary. */
            if ( batch_view )
                push.dbg_tint = batch_debug_color( draw_calls );
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

    /* Draw calls are this surface's own partition; sum them across surfaces into the batch total.
       (Geometry stats are recorded once in render_build_frame -- the list is shared.) */
    s_render.accum.draw_calls += draw_calls;
    static u32 prev_draw_calls = ~0u;   /* sentinel: forces a print on the first frame */
    if ( s_render_debug_draw_calls && draw_calls != prev_draw_calls )
    {
        printf( "[imgui] draw calls this frame: %u (peak %u)\n", draw_calls, s_render.draw_call_hwm );
        prev_draw_calls = draw_calls;
    }
}

// clang-format on
/*============================================================================================*/
