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
    s_frame_built; subsequent surfaces that frame reuse the slot table and only re-do the cheap
    per-surface part (upload and draw).  imgui_render_frame_reset clears the guard at frame_begin.
----------------------------------------------------------------------------------------------*/

static bool s_frame_built;   /* slot table + tessellation computed this frame */

/*----------------------------------------------------------------------------------------------
    Per-window geometry slot system (Level 2c -- retained geometry).

    Each window gets one slot that caches its tessellated geometry (vertices, indices, GPU draw
    commands).  Unchanged windows copy their geometry from the previous frame's buffer instead of
    re-tessellating.  Slots are sorted by win->z for dispatch; the flush issues one group of draw
    calls per slot, using vertex_offset = slot.vert_base so indices stay 0-relative within the
    slot and remain valid regardless of where the slot lands in the shared VB this frame.

    WIN_SLOT_CMD_MAX caps the GPU draw commands per window (one per clip+texture change within the
    window).  32 is generous; most windows have 2-4.  Overflow falls back to natural emit order
    (correct, just less merged) -- same as render_tess_window's clip overflow path.
----------------------------------------------------------------------------------------------*/

#define RENDER_MAX_WIN   256   /* distinct windows tracked per frame (slots + cache diff) */
#define WIN_SLOT_CMD_MAX  16   /* max GPU draw cmds cached per slot; most windows have 2-4 */

/* AOS record for one cached GPU draw command.  Packed together so replaying a slot's cached
   commands touches one contiguous region instead of four parallel arrays. */
typedef struct
{
    imgui_gpu_cmd_t cmd;     /* clip rect, texture slot, element count     */
    u32             z;       /* sort key this command was emitted under     */
    u32             vp;      /* viewport this command targets               */
    u32             lvbase;  /* slot-local (0-relative) vertex base         */
} win_slot_cmd_t;

typedef struct
{
    imgui_id_t      win;
    u32             z, vp;
    u32             vert_base,  vert_count;
    u32             idx_base,   idx_count;
    u32             cmd_base,   cmd_count;    /* range into s_tess.cmds[] */
    win_slot_cmd_t  cached[ WIN_SLOT_CMD_MAX ];  /* GPU cmds; filled at tess, replayed on reuse */
    bool            valid;                    /* geometry tessellated at least once */
} win_geo_slot_t;

static win_geo_slot_t  s_slots     [ RENDER_MAX_WIN ];    /* current frame's per-window slots  */
static win_geo_slot_t  s_slots_prev[ RENDER_MAX_WIN ];    /* previous frame's slots (for reuse) */
static u32             s_slot_count, s_slot_prev_count;
static win_geo_slot_t* s_dispatch  [ RENDER_MAX_WIN ];    /* z-sorted pointers into s_slots    */
static u32             s_dispatch_count;

/* Previous-frame geometry buffers.  Copied from s_tess at end of render_build_frame and used by
   the reuse path to memcpy unchanged window geometry into the current frame's s_tess without
   re-running any tessellation math.  Indices are 0-relative within each slot so the copy is
   direct -- no fixup needed when the slot lands at a different absolute position. */
static imgui_draw_vert_t s_geo_prev_verts  [ IMGUI_MAX_VERTS ];
static u16               s_geo_prev_indices[ IMGUI_MAX_IDX   ];

/* Manual debug toggle: flip to true (debugger, or at startup) to print the per-frame
   draw-call count every flush.  The high-water mark is always tracked and reported at
   shutdown regardless of this flag. */
static bool s_render_debug_draw_calls = false;

/* Manual debug toggle: flip to true to print the per-frame emitted geometry (vertex /
   index counts) every flush -- a direct read on render density, so the effect of UI state
   changes (e.g. collapsing a window) is visible live in the console.  Peaks are tracked in
   s_draw regardless of this flag and reported at shutdown. */
static bool s_render_debug_geometry = false;

/* Manual debug toggle: flip to true to print the per-frame retained-cache diff -- how many windows
   were unchanged since last frame (their command bytes hashed identical) vs the total. */
static bool s_render_debug_cache = false;

/* Manual debug toggle: flip to true to print per-frame retained-geometry stats -- how many windows
   and how many vertices/triangles were reused from the previous frame vs total.  The same numbers
   are available live through imgui()->render_stats() at mode >= 4 in the perf overlay. */
static bool s_render_debug_retained = true;

/* Retained-skip optimization: when true and the diff says nothing changed, tessellation is skipped
   and s_tess retains the previous frame's geometry for the flush.  Toggled via
   imgui_render_set_retained_skip / imgui()->set_retained_skip (key C in sb_vulkan).
   Default on; disable to benchmark or confirm correctness of the hash-upfront path. */
static bool s_retained_skip = true;

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
    imgui_render_set_retained_skip / imgui_render_retained_skip -- toggle the tess-skip
    optimization.  When on (default) and the per-window hash diff reports no change, the
    tessellation pass is skipped and the previous frame's geometry is reused.
----------------------------------------------------------------------------------------------*/

void
imgui_render_set_retained_skip( bool on )
{
    s_retained_skip = on;
}

bool
imgui_render_retained_skip( void )
{
    return s_retained_skip;
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

/* rect_empty (a zero-area clip) is defined in imgui_draw.c, included above this unit. */

#define RENDER_MAX_CLIP_GROUPS  64   /* distinct clip rects groupable within one window */

/*----------------------------------------------------------------------------------------------
    render_tess_window -- tessellate all semantic commands belonging to one window.

    Gathers this window's segments from s_draw.segs, builds a clip-sorted permutation into a
    local stack array (no global order buffer needed), then calls tess_dispatch.  Grouping by
    clip within the window follows the same first-seen strategy as the old global ordering:
    distinct clips are collected in emit order and replayed group by group so equal-clip shapes
    tessellate back-to-back and merge into one GPU batch.  z sorting no longer happens here:
    each window occupies a slot whose dispatch z is the window's raise z; the caller z-sorts the
    slot table before issuing draw calls, keeping all of a window's geometry (chrome + body)
    together in one contiguous slot regardless of their internal z values.
----------------------------------------------------------------------------------------------*/

static void
render_tess_window( imgui_id_t win )
{
    const imgui_cmd_seg_t* segs = s_draw.segs;
    u32                    nseg = s_draw.seg_count;

    /* Distinct clips for this window's commands, first-seen order. */
    imgui_rect_t clips[ RENDER_MAX_CLIP_GROUPS ];
    u32          nc       = 0;
    bool         overflow = false;

    for ( u32 si = 0; si < nseg && !overflow; ++si )
    {
        if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
        {
            const imgui_cmd_t* c = &s_draw.cmds[ i ];
            if ( rect_empty( c->clip ) ) continue;
            bool seen = false;
            for ( u32 g = 0; g < nc; ++g )
                if ( clip_eq( clips[ g ], c->clip ) ) { seen = true; break; }
            if ( !seen )
            {
                if ( nc >= RENDER_MAX_CLIP_GROUPS ) { overflow = true; break; }
                clips[ nc++ ] = c->clip;
            }
        }
    }

    /* Clip-sorted permutation into a local buffer (upper bound is the whole command list). */
    u32 win_order[ IMGUI_MAX_CMDS ];
    u32 n = 0;

    if ( overflow )
    {
        /* Too many distinct clips: natural emit order (correct, just less merged). */
        for ( u32 si = 0; si < nseg; ++si )
        {
            if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
            for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                if ( !rect_empty( s_draw.cmds[ i ].clip ) )
                    win_order[ n++ ] = i;
        }
    }
    else
    {
        for ( u32 g = 0; g < nc; ++g )
            for ( u32 si = 0; si < nseg; ++si )
            {
                if ( segs[ si ].win != win || segs[ si ].lo == segs[ si ].hi ) continue;
                for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
                    if ( clip_eq( clips[ g ], s_draw.cmds[ i ].clip ) )
                        win_order[ n++ ] = i;
            }
    }

    tess_dispatch( s_draw.cmds, win_order, n );
}

/*----------------------------------------------------------------------------------------------
    Retained-cache change detection (Level 2, detection half).

    Each frame we hash every window's emitted commands -- keyed by the stable window id the emit path
    stamps onto its segments -- and compare against last frame's hashes (double-buffered).  A window
    whose hash matches is unchanged: next, its tessellated geometry can be reused instead of rebuilt
    (not wired yet).  any_changed is the coarse signal (a window appeared, vanished, or changed).

    The command hash is deliberately DEEP for the two command types that point into side pools: TEXT
    (text_pool by offset) and POLYLINE (points by offset).  A same-length, same-offset label edit
    ("3" -> "4") leaves the 72-byte command struct byte-identical, so without folding the pooled bytes
    the change would be missed.  Conversely only the ACTIVE union member is hashed: cmds[] is reused
    across frames, so the stale tail of a slot's previous (larger) command must not perturb the hash.
----------------------------------------------------------------------------------------------*/

/* Per-window record for the cache diff.  cur[] is rebuilt every frame; prev[] is the snapshot
   from the previous frame.  z/vp are the max-z and last-vp across the window's segments, computed
   during the diff pass so the per-window slot build needs no second segment scan.  changed is the
   result of the hash comparison, stored inline so no separate parallel array is needed. */
typedef struct
{
    imgui_id_t win;
    u32        hash;
    u32        z;        /* max segment z this frame (used for slot dispatch order) */
    u32        vp;       /* viewport of the last segment this frame                */
    bool       changed;  /* hash mismatched or window is new                       */
} render_win_hash_t;

static struct
{
    render_win_hash_t cur [ RENDER_MAX_WIN ];   /* this frame's per-window records     */
    render_win_hash_t prev[ RENDER_MAX_WIN ];   /* last frame's hashes, for the diff   */
    u32  cur_n, prev_n;
    u32  unchanged;                     /* windows whose hash matched last frame        */
    bool any_changed;                   /* a window appeared / vanished / changed       */
} s_cache;

/*----------------------------------------------------------------------------------------------
    render_build_cache_diff -- fold pre-baked per-command hashes into per-window hashes and
    diff against the previous frame.

    Per-command hashes are computed at emit time (draw_hash_cmd in imgui_draw.c) while the
    command data is still L1-hot.  This function just folds s_draw.cmd_hashes[lo..hi] per
    segment -- no switch, no pool pointer chasing.  It runs BEFORE tessellation so a fully
    unchanged frame can skip tess entirely (the s_retained_skip path).
----------------------------------------------------------------------------------------------*/

static void
render_build_cache_diff( void )
{
    const imgui_cmd_seg_t* segs = s_draw.segs;
    u32                    nseg = s_draw.seg_count;

    /* Roll each segment's pre-baked command hashes into its window's accumulated hash.
       Also accumulate max-z and last-vp per window here, so the slot build loop needs no
       second pass over segments.  Segments of one window arrive in increasing lo (emit order)
       so the hash fold and z/vp accumulation are both stable. */
    s_cache.cur_n = 0;
    for ( u32 si = 0; si < nseg; ++si )
    {
        if ( segs[ si ].lo == segs[ si ].hi ) continue;   /* empty span */

        imgui_id_t win = segs[ si ].win;
        u32        bi  = 0;
        for ( ; bi < s_cache.cur_n; ++bi )
            if ( s_cache.cur[ bi ].win == win ) break;
        if ( bi == s_cache.cur_n )
        {
            if ( s_cache.cur_n >= RENDER_MAX_WIN ) continue;   /* overflow: treated as changed */
            s_cache.cur[ bi ].win  = win;
            s_cache.cur[ bi ].hash = 2166136261u;
            s_cache.cur[ bi ].z    = 0;
            s_cache.cur[ bi ].vp   = 0;
            ++s_cache.cur_n;
        }

        /* Fold sort key and viewport into hash so a raise or surface move invalidates the window. */
        u32 h = s_cache.cur[ bi ].hash;
        h = fnv1a( h, &segs[ si ].z,  sizeof segs[ si ].z );
        h = fnv1a( h, &segs[ si ].vp, sizeof segs[ si ].vp );
        for ( u32 i = segs[ si ].lo; i < segs[ si ].hi; ++i )
            h = fnv1a( h, &s_draw.cmd_hashes[ i ], sizeof s_draw.cmd_hashes[ i ] );
        s_cache.cur[ bi ].hash = h;

        /* Track max-z (dispatch order) and last vp (surface routing) for this window. */
        if ( segs[ si ].z > s_cache.cur[ bi ].z )
            s_cache.cur[ bi ].z  = segs[ si ].z;
        s_cache.cur[ bi ].vp = segs[ si ].vp;
    }

    /* Diff against last frame: unchanged iff the window existed with the same hash. */
    s_cache.unchanged   = 0;
    s_cache.any_changed = ( s_cache.cur_n != s_cache.prev_n );
    for ( u32 i = 0; i < s_cache.cur_n; ++i )
    {
        bool match = false;
        for ( u32 j = 0; j < s_cache.prev_n; ++j )
            if ( s_cache.prev[ j ].win == s_cache.cur[ i ].win )
            {
                match = ( s_cache.prev[ j ].hash == s_cache.cur[ i ].hash );
                break;
            }
        s_cache.cur[ i ].changed = !match;
        if ( match ) ++s_cache.unchanged;
        else         s_cache.any_changed = true;
    }

    /* Promote this frame's table to prev for next frame's diff (only win+hash needed in prev). */
    memcpy( s_cache.prev, s_cache.cur, s_cache.cur_n * sizeof( render_win_hash_t ) );
    s_cache.prev_n = s_cache.cur_n;

    if ( s_render_debug_cache && s_cache.any_changed )
        printf( "[imgui] cache: frame changed -- %u/%u windows unchanged\n",
                s_cache.unchanged, s_cache.cur_n );
}

/*----------------------------------------------------------------------------------------------
    render_build_frame -- tessellate the shared command list and build the sorted run table.

    Runs once per frame (guarded by s_frame_built): the first surface flush triggers it, every
    other surface that frame reuses s_tess + s_dispatch.  Produces the geometry (s_tess.verts/indices),
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

    /* Close the still-open final segment so the diff and per-window tess both see its full [lo,hi). */
    if ( s_draw.seg_count > 0 )
        s_draw.segs[ s_draw.seg_count - 1 ].hi = s_draw.cmd_count;

    /* Diff: fold pre-baked per-command hashes into per-window records and compare against last frame.
       Populates s_cache.cur[] (with hash, z, vp, changed) and s_cache.any_changed.  Runs before
       tess so per-window z/vp/changed are ready for the slot loop without a second segment scan. */
    render_build_cache_diff();

    /* Swap slot tables: prev <- cur, then reset current for this frame's build. */
    memcpy( s_slots_prev, s_slots, s_slot_count * sizeof( win_geo_slot_t ) );
    s_slot_prev_count = s_slot_count;
    s_slot_count      = 0;
    s_dispatch_count  = 0;

    /* Always reset tess: even a fully-unchanged frame rebuilds s_tess from the per-window loop
       (unchanged windows contribute memcpy'd geometry, changed ones run tess).  slot_vert_base is
       reset per-slot inside the loop. */
    tess_reset();

    /* Per-window loop: build one geometry slot per unique window in s_cache.cur order.
       z and vp were pre-computed by render_build_cache_diff, so no second segment scan is needed.
       Unchanged windows (s_retained_skip on + hash matched + prev slot valid) copy geometry from
       s_geo_prev_{verts,indices} and replay their cached GPU draw commands.  Everything else runs
       render_tess_window.  The slot table ends up in emit order; a z-sort on s_dispatch[] puts
       them back-to-front for the flush. */
    u32 vert_retained = 0, tri_retained = 0, win_retained = 0;

    for ( u32 wi = 0; wi < s_cache.cur_n; ++wi )
    {
        const render_win_hash_t* wh   = &s_cache.cur[ wi ];
        win_geo_slot_t*          slot = &s_slots[ s_slot_count++ ];

        slot->win   = wh->win;
        slot->z     = wh->z;    /* pre-computed during diff: max segment z */
        slot->vp    = wh->vp;   /* pre-computed during diff: last segment vp */
        slot->valid = false;

        /* Try to find a valid previous-frame slot for geometry reuse. */
        win_geo_slot_t* prev = NULL;
        if ( s_retained_skip && !wh->changed )
        {
            for ( u32 pi = 0; pi < s_slot_prev_count; ++pi )
                if ( s_slots_prev[ pi ].win == wh->win && s_slots_prev[ pi ].valid )
                {
                    prev = &s_slots_prev[ pi ];
                    break;
                }
        }

        if ( prev )
        {
            /* Unchanged window: copy geometry from the previous frame's buffers directly.
               Indices are 0-relative within each slot so the copy is correct regardless of
               the slot's new absolute position in the shared VB.  vertex_offset in the draw
               call shifts them to slot->vert_base at submit time. */
            slot->vert_base  = s_tess.vert_count;
            slot->vert_count = prev->vert_count;
            slot->idx_base   = s_tess.idx_count;
            slot->idx_count  = prev->idx_count;
            slot->cmd_base   = s_tess.cmd_count;
            slot->cmd_count  = prev->cmd_count;

            if ( slot->vert_count > 0 )
                memcpy( &s_tess.verts[ s_tess.vert_count ],
                        &s_geo_prev_verts[ prev->vert_base ],
                        slot->vert_count * sizeof( imgui_draw_vert_t ) );
            s_tess.vert_count += slot->vert_count;

            if ( slot->idx_count > 0 )
                memcpy( &s_tess.indices[ s_tess.idx_count ],
                        &s_geo_prev_indices[ prev->idx_base ],
                        slot->idx_count * sizeof( u16 ) );
            s_tess.idx_count += slot->idx_count;

            /* Replay cached GPU draw commands; carry them forward for future reuse. */
            u32 nc = prev->cmd_count;   /* already capped at WIN_SLOT_CMD_MAX when stored */
            for ( u32 k = 0; k < nc; ++k )
            {
                u32 ci = slot->cmd_base + k;
                s_tess.cmds    [ ci ] = prev->cached[ k ].cmd;
                s_tess.cmd_z   [ ci ] = prev->cached[ k ].z;
                s_tess.cmd_vp  [ ci ] = prev->cached[ k ].vp;
                s_tess.cmd_vbase[ ci ] = slot->vert_base + prev->cached[ k ].lvbase;
            }
            s_tess.cmd_count += nc;

            memcpy( slot->cached, prev->cached, nc * sizeof( win_slot_cmd_t ) );

            vert_retained += slot->vert_count;
            tri_retained  += slot->idx_count / 3u;
            ++win_retained;

            slot->valid = true;
        }
        else
        {
            /* Changed or new window: tessellate fresh. */
            slot->vert_base        = s_tess.vert_count;
            slot->idx_base         = s_tess.idx_count;
            slot->cmd_base         = s_tess.cmd_count;
            s_tess.slot_vert_base  = s_tess.vert_count;   /* indices emitted as slot-local offsets */

            render_tess_window( wh->win );

            slot->vert_count = s_tess.vert_count - slot->vert_base;
            slot->idx_count  = s_tess.idx_count  - slot->idx_base;
            slot->cmd_count  = s_tess.cmd_count  - slot->cmd_base;

            /* Cache GPU draw commands for potential reuse next frame. */
            u32 nc = slot->cmd_count;
            if ( nc > WIN_SLOT_CMD_MAX ) nc = WIN_SLOT_CMD_MAX;
            for ( u32 k = 0; k < nc; ++k )
            {
                u32 ci = slot->cmd_base + k;
                slot->cached[ k ].cmd    = s_tess.cmds    [ ci ];
                slot->cached[ k ].z      = s_tess.cmd_z   [ ci ];
                slot->cached[ k ].vp     = s_tess.cmd_vp  [ ci ];
                slot->cached[ k ].lvbase = s_tess.cmd_vbase[ ci ] - slot->vert_base;
            }
            slot->cmd_count = nc;   /* capped; extra GPU cmds beyond WIN_SLOT_CMD_MAX are not cached */
            slot->valid     = true;
        }

        s_dispatch[ s_dispatch_count++ ] = slot;
    }

    /* Insertion sort dispatch pointers by slot->z ascending so slots draw back-to-front. */
    for ( u32 a = 1; a < s_dispatch_count; ++a )
    {
        win_geo_slot_t* key = s_dispatch[ a ];
        u32 b = a;
        while ( b > 0 && s_dispatch[ b - 1 ]->z > key->z )
        {
            s_dispatch[ b ] = s_dispatch[ b - 1 ];
            --b;
        }
        s_dispatch[ b ] = key;
    }

    /* Save this frame's geometry into the prev buffers so unchanged windows next frame can
       memcpy from them.  Skip when nothing changed: in that case s_tess is byte-for-byte
       identical to the prev buffers (all geometry was copied from them), so copying back
       would be wasted work -- a win on fully idle frames (320 KB + 96 KB skipped). */
    if ( s_cache.any_changed )
    {
        if ( s_tess.vert_count > 0 )
            memcpy( s_geo_prev_verts, s_tess.verts, s_tess.vert_count * sizeof( imgui_draw_vert_t ) );
        if ( s_tess.idx_count > 0 )
            memcpy( s_geo_prev_indices, s_tess.indices, s_tess.idx_count * sizeof( u16 ) );
    }

    /* Retained stats -- tracked per frame, published via imgui_render_stats(). */
    s_render.accum.win_total    = s_cache.cur_n;
    s_render.accum.win_retained = win_retained;
    s_render.accum.vert_retained = vert_retained;
    s_render.accum.tri_retained  = tri_retained;

    /* Track peak tessellated geometry.  Warn once on the first overflow. */
    if ( s_tess.vert_count > s_tess.vert_hwm ) s_tess.vert_hwm = s_tess.vert_count;
    if ( s_tess.idx_count  > s_tess.idx_hwm  ) s_tess.idx_hwm  = s_tess.idx_count;

    if ( s_tess.overflow && !s_tess.overflow_ever )
        printf( "[imgui] WARNING: draw list overflow -- geometry dropped this frame "
                "(verts capped at %u, idx capped at %u). Raise IMGUI_MAX_VERTS / IMGUI_MAX_IDX.\n",
                IMGUI_MAX_VERTS, IMGUI_MAX_IDX );
    if ( s_tess.overflow )
        s_tess.overflow_ever = true;

    /* Stats. */
    s_render.accum.cmd_count  = s_draw.cmd_count;
    s_render.accum.vert_count = s_tess.vert_count;
    s_render.accum.tri_count  = s_tess.idx_count / 3u;

    static u32 prev_verts = ~0u, prev_idx = ~0u;
    if ( s_render_debug_geometry && ( s_tess.vert_count != prev_verts || s_tess.idx_count != prev_idx ) )
    {
        printf( "[imgui] geometry this frame: verts %u/%u (peak %u), idx %u/%u (peak %u)\n",
                s_tess.vert_count, IMGUI_MAX_VERTS, s_tess.vert_hwm,
                s_tess.idx_count,  IMGUI_MAX_IDX,   s_tess.idx_hwm );
        prev_verts = s_tess.vert_count;
        prev_idx   = s_tess.idx_count;
    }

    static u32 prev_win_ret = ~0u, prev_vert_ret = ~0u;
    if ( s_render_debug_retained &&
         ( s_render.accum.win_retained  != prev_win_ret ||
           s_render.accum.vert_retained != prev_vert_ret ) )
    {
        printf( "[imgui] retained: wins %u/%u  verts %u/%u  tris %u/%u\n",
                s_render.accum.win_retained, s_render.accum.win_total,
                s_render.accum.vert_retained, s_render.accum.vert_count,
                s_render.accum.tri_retained,  s_render.accum.tri_count );
        prev_win_ret  = s_render.accum.win_retained;
        prev_vert_ret = s_render.accum.vert_retained;
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

    /* Compute this surface's vertex + index upload span from the slot table.  Slots tagged for this
       viewport contribute their [vert_base, vert_base+vert_count) and [idx_base, idx_base+idx_count)
       ranges; the union is what we upload.  For a single surface this covers the entire buffer. */
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

    push.samp_idx = s_render.font_sampler_idx;

    /* Debug render mode push state.  dbg_flat makes the fragment bypass the atlas and emit a flat
       color: WIREFRAME keeps each window's vertex color (tint 0), BATCH overrides it per draw call
       with a distinct palette color below.  NORMAL leaves both 0 for the textured/blended path. */
    bool batch_view = ( s_render.debug_mode == IMGUI_RENDER_BATCH );
    push.dbg_flat   = ( s_render.debug_mode == IMGUI_RENDER_NORMAL ) ? 0u : 1u;
    push.dbg_tint   = 0u;

    u32 draw_calls = 0;   /* indexed draws actually emitted this frame (one per non-empty command) */

    /* Iterate s_dispatch[] (z-sorted slot pointers) back-to-front.  Each slot owns a contiguous
       region of s_tess.verts[] and s_tess.indices[]; its GPU draw commands reference those regions
       via 0-relative indices + vertex_offset = slot->vert_base.  Slots for other viewports are
       skipped entirely; within a slot, individual commands with a mismatched vp are stepped over
       (their index range must still be consumed to keep first_index aligned). */
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* slot = s_dispatch[ d ];
        if ( slot->vp != vp_index )
            continue;

        u32 first_index = slot->idx_base;   /* absolute start of this slot's indices in the IB */
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

            /* Scissor to the draw command's clip rect.  Floor the origin and ceil the far edge so a
               fractional clip rect never rounds inward and shaves a pixel off visible content. */
            i32 sx0 = (i32)floorf( dc->clip_rect.x );
            i32 sy0 = (i32)floorf( dc->clip_rect.y );
            i32 sx1 = (i32)ceilf ( dc->clip_rect.x + dc->clip_rect.w );
            i32 sy1 = (i32)ceilf ( dc->clip_rect.y + dc->clip_rect.h );

            /* Clamp to framebuffer bounds (Vulkan requires offset >= 0 and extent within the surface). */
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
                .vertex_offset  = (i32)slot->vert_base,   /* slot-local indices + vert_base = absolute */
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
