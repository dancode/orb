/*==============================================================================================

    sandbox/vulkan_stress/sb_vulkan_stress.c -- Multi-context RHI synchronization stress test.

    Separate testbed from sb_vulkan.  Where sb_vulkan brings up a single window to develop the
    Vulkan pipeline (and imgui on top of it), this sandbox deliberately hammers the parts of the
    RHI that are easy to get subtly wrong:

        1. Multiple windows, each with its own rhi context (its own swapchain, per-frame command
           pools, fences and semaphores).  All contexts are driven from a single host loop so the
           per-context sync primitives are exercised concurrently every frame.

        2. Aggressive asset churn.  Every frame each context creates a batch of transient GPU
           resources (a GPU_ONLY buffer + a GPU_ONLY texture, both filled via the staged upload
           path) and frees aged ones.  The staged upload is tied to frames-in-flight, so the
           create/use/free cadence stresses whether buffer_destroy / texture_destroy correctly
           wait for any in-flight frame (or pending staging copy) that still references the
           resource before actually releasing it.

    There is no imgui, no draw service, and no hot-reload here -- this is purely about proving the
    RHI context + resource lifetime + synchronization machinery holds up under load.

    Controls:
        ESC      quit
        F1       toggle aggressive mode (free assets the same frame they are created)
        F2       print live asset / sync stats
        + / -    raise / lower per-context asset churn each frame

==============================================================================================*/

#include <stdio.h>
#include <math.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"

/* F4 sampled mode reuses imgui's pre-compiled bindless SPIR-V (self-contained u32 arrays:
   s_imgui_vert_spirv / s_imgui_frag_spirv).  This is bytecode only -- no imgui library linkage --
   so the stress test stays independent of imgui while still driving a real textured draw. */
#include "runtime_service/imgui/imgui_shader.h"

// clang-format off

/*==============================================================================================
    Tunables
==============================================================================================*/

#define STRESS_WINDOW_COUNT     RHI_CTX_MAX          /* one rhi context per window (max 4)      */
#define STRESS_CHURN_DEFAULT    8                    /* transient assets created per ctx / frame */
#define STRESS_CHURN_MAX        64
#define STRESS_TEX_DIM          8                    /* tiny upload texture (8x8 RGBA8)          */

/* Default lifetime (in host frames) before a transient asset is freed.  Kept above
   RHI_MAX_FRAMES_IN_FLIGHT so the "correct" path normally never frees a resource the GPU
   could still be touching -- aggressive mode (F1) drops this to 0 to probe the deferred-free. */
#define STRESS_FREE_DELAY       ( RHI_MAX_FRAMES_IN_FLIGHT + 1 )

/* F3 context churn: how many host frames between recycling one window's rhi context (destroy +
   recreate on the same OS window).  Stresses the ctx_alloc / epoch_ack_mask bitmask machinery and
   garbage/descriptor reclaim across a changing live-context count, while siblings keep rendering. */
#define STRESS_CTX_CHURN_INTERVAL  20

/* F4 sampled mode: textures that are uploaded, registered bindless, and actually SAMPLED by a
   real draw -- exercising the QFOT acquire -> shader-read path end to end, plus bindless slot
   register/unregister churn and deferred-destroy of a texture in-flight frames are sampling. */
#define STRESS_SAMPLED_MAX      12                   /* live sampled textures at once (grid cells) */
#define STRESS_SAMPLED_DIM      16                   /* sampled texture size (checker pattern)     */
#define STRESS_SAMPLED_LIFETIME 24                   /* host frames before a sampled tex is retired */

/*==============================================================================================
    Per-window render context
==============================================================================================*/

typedef struct
{
    win_id_t win;     /* app window id                                  */
    i32      ctx;     /* rhi context id (RHI_CTX_INVALID when unused)    */
    i32      w, h;    /* current drawable size                          */
    f32      phase;   /* clear-color animation offset, unique per window */
    bool     open;    /* false once the window has been closed          */

} stress_win_t;

static stress_win_t s_wins[ STRESS_WINDOW_COUNT ];

/*==============================================================================================
    Transient asset ring

    A simple FIFO of recently created resources.  Each frame we push freshly created assets onto
    the tail and pop aged ones off the head.  born_frame records the host frame the asset was
    created in; an asset is freed once ( frame_now - born_frame ) >= free_delay.
==============================================================================================*/

typedef struct
{
    rhi_buffer_t  buf;
    rhi_texture_t tex;
    u64           born_frame;

} stress_asset_t;

#define STRESS_ASSET_CAP  4096                       /* >> churn_max * (free_delay + margin)    */

static stress_asset_t s_assets[ STRESS_ASSET_CAP ];
static u32            s_asset_head;                  /* index of oldest live asset              */
static u32            s_asset_count;                 /* number of live assets                   */

/* Running stats. */
static u64 s_total_created;
static u64 s_total_freed;
static u32 s_peak_live;

/*==============================================================================================
    Asset create / free

    create_asset allocates a GPU_ONLY buffer and a GPU_ONLY texture, then fills both through the
    staged upload path (upload_buffer / upload_texture).  That path defers the actual copy until
    a later frame_begin, which is precisely what makes premature destruction dangerous -- exactly
    the case this sandbox is built to flush out.
==============================================================================================*/

static bool
create_asset( u64 frame_now )
{
    if ( s_asset_count >= STRESS_ASSET_CAP )
        return false;   /* ring full -- aging is not keeping up; skip rather than overwrite */

    /* GPU_ONLY buffer filled via staging. */
    static const u32 buf_data[ 16 ] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
        0x55555555, 0x66666666, 0x77777777, 0x88888888,
        0x99999999, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC,
        0xDDDDDDDD, 0xEEEEEEEE, 0xFFFFFFFF, 0x00000000,
    };

    rhi_buffer_t buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = sizeof( buf_data ),
        .usage      = RHI_BUFFER_USAGE_STORAGE | RHI_BUFFER_USAGE_TRANSFER_DST,
        .memory     = RHI_MEMORY_GPU_ONLY,
        .debug_name = "stress_buf",
    } );
    if ( !rhi_handle_valid( buf ) )
        return false;

    if ( !rhi()->upload_buffer( buf, buf_data, sizeof( buf_data ) ) )
    {
        /* Staging pool exhausted -- back out the buffer so we do not leak it. */
        rhi()->buffer_destroy( buf );
        return false;
    }

    /* GPU_ONLY texture filled via staging. */
    static u8 tex_data[ STRESS_TEX_DIM * STRESS_TEX_DIM * 4 ];
    for ( u32 i = 0; i < sizeof( tex_data ); ++i )
        tex_data[ i ] = (u8)( i * 7 + 13 );

    rhi_texture_t tex = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = STRESS_TEX_DIM,
        .height       = STRESS_TEX_DIM,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_RGBA8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = "stress_tex",
    } );
    if ( !rhi_handle_valid( tex ) )
    {
        rhi()->buffer_destroy( buf );
        return false;
    }

    if ( !rhi()->upload_texture( tex, tex_data, sizeof( tex_data ), 0, 0 ) )
    {
        rhi()->texture_destroy( tex );
        rhi()->buffer_destroy( buf );
        return false;
    }

    /* Push onto the FIFO tail. */
    u32 slot = ( s_asset_head + s_asset_count ) % STRESS_ASSET_CAP;
    s_assets[ slot ] = ( stress_asset_t ){ .buf = buf, .tex = tex, .born_frame = frame_now };
    ++s_asset_count;

    ++s_total_created;
    if ( s_asset_count > s_peak_live )
        s_peak_live = s_asset_count;

    return true;
}

/* Free the oldest asset.  This is where the RHI must do the right thing: the resource may have
   been uploaded only a frame or two ago and still be referenced by an in-flight frame. */
static void
free_oldest( void )
{
    stress_asset_t* a = &s_assets[ s_asset_head ];

    rhi()->texture_destroy( a->tex );
    rhi()->buffer_destroy( a->buf );

    *a = ( stress_asset_t ){ 0 };
    s_asset_head = ( s_asset_head + 1 ) % STRESS_ASSET_CAP;
    --s_asset_count;
    ++s_total_freed;
}

/* Age out everything past its lifetime (aggressive mode passes free_delay = 0 to free same-frame). */
static void
sweep_assets( u64 frame_now, u64 free_delay )
{
    while ( s_asset_count > 0 )
    {
        const stress_asset_t* a = &s_assets[ s_asset_head ];
        if ( ( frame_now - a->born_frame ) < free_delay )
            break;   /* FIFO: once one is too young, all newer ones are too */
        free_oldest();
    }
}

/* Drain everything (used at shutdown, after the GPU has been idled by context teardown). */
static void
free_all_assets( void )
{
    while ( s_asset_count > 0 )
        free_oldest();
}

/*==============================================================================================
    Window helpers
==============================================================================================*/

static stress_win_t*
find_win( win_id_t id )
{
    for ( u32 i = 0; i < STRESS_WINDOW_COUNT; ++i )
        if ( s_wins[ i ].open && s_wins[ i ].win == id )
            return &s_wins[ i ];
    return NULL;
}

static u32
open_window_count( void )
{
    u32 n = 0;
    for ( u32 i = 0; i < STRESS_WINDOW_COUNT; ++i )
        n += s_wins[ i ].open ? 1 : 0;
    return n;
}

/* Tear down a single window's rhi context (idles GPU) and close its OS window. */
static void
close_window( stress_win_t* sw )
{
    if ( !sw->open )
        return;

    if ( sw->ctx != RHI_CTX_INVALID )
    {
        rhi()->context_destroy( sw->ctx );   /* waits for GPU idle on this context */
        sw->ctx = RHI_CTX_INVALID;
    }
    app()->window_close( sw->win );
    sw->open = false;
}

/*==============================================================================================
    Recycle one window's rhi context (F3 churn): destroy + recreate on the same OS window.

    This is the corner case sb_vulkan_stress could not hit while holding 4 fixed contexts: a live
    context count that changes at runtime.  context_destroy idles that context's GPU work and clears
    its bit from ctx_alloc / epoch_ack_mask; context_create allocates a fresh slot (often the same
    index).  Siblings keep rendering throughout, so the epoch must still converge across the change,
    and any QFOT acquire that was about to land in the destroyed context must be picked up elsewhere.
==============================================================================================*/

static void
recycle_context( stress_win_t* sw )
{
    if ( !sw->open || sw->ctx == RHI_CTX_INVALID )
        return;

    /* A minimized window reports a zero surface extent; context_create would just defer its
       swapchain anyway, so skip recycling it this round and try again once it is restored. */
    if ( app()->window_is_minimized( sw->win ) )
        return;

    rhi()->context_destroy( sw->ctx );    /* idles this context's GPU work, frees its slot */
    sw->ctx = RHI_CTX_INVALID;

    void* hwnd = app()->window_handle( sw->win );
    i32   ctx  = rhi()->context_create( sw->win, hwnd, sw->w, sw->h );
    if ( ctx == RHI_CTX_INVALID )
    {
        /* Leave the window open with an invalid ctx; the render loop skips it until a later
           recycle succeeds.  (Should not happen on a valid, non-minimized window.) */
        fprintf( stderr, "[sb_vulkan_stress] recycle: context_create failed for win %d\n", sw->win );
        return;
    }
    sw->ctx = ctx;
}

/*==============================================================================================
    F4 sampled mode -- upload + register bindless + sample in a real draw.

    Shares one pipeline / sampler / unit-quad VB (built once via f4_init) across all windows, and
    a small pool of "live" textures.  Each is created GPU_ONLY, filled through the staged upload
    path, registered in the bindless set, drawn for a few frames as a grid quad that samples it,
    then retired (unregister + deferred texture_destroy).  This is the only place the stress test
    consumes an uploaded texture with an actual shader read, so it validates that the QFOT acquire
    really makes the image SHADER_READ_ONLY before it is sampled -- and that retiring a texture
    that in-flight frames are still sampling defers correctly.
==============================================================================================*/

/* Vertex + push layouts must match imgui's shaders (stride 20; push 72 bytes). */
typedef struct { f32 x, y, u, v; u32 abgr; } f4_vert_t;
typedef struct { f32 mvp[ 16 ]; u32 tex_idx; u32 samp_idx; } f4_push_t;

typedef struct
{
    rhi_texture_t tex;
    u32           idx;    /* bindless texture slot */
    u64           born;   /* host frame created    */
    bool          live;

} f4_sampled_t;

static rhi_pipeline_t s_f4_pipeline;
static rhi_sampler_t  s_f4_sampler;
static u32            s_f4_sampler_idx;
static rhi_buffer_t   s_f4_quad_vb;
static f4_sampled_t   s_f4_sampled[ STRESS_SAMPLED_MAX ];

/* Build the shared GPU resources.  Returns false (F4 stays unavailable) on any failure. */
static bool
f4_init( void )
{
    rhi_shader_t vert = rhi()->shader_load_memory( s_imgui_vert_spirv, sizeof( s_imgui_vert_spirv ),
                                                   RHI_SHADER_STAGE_VERTEX, "main", "f4_vert" );
    if ( !rhi_handle_valid( vert ) )
        return false;

    rhi_shader_t frag = rhi()->shader_load_memory( s_imgui_frag_spirv, sizeof( s_imgui_frag_spirv ),
                                                   RHI_SHADER_STAGE_FRAGMENT, "main", "f4_frag" );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
        return false;
    }

    rhi_vertex_attrib_t attribs[ 3 ] = {
        { .binding = 0, .location = 0, .offset =  0, .format = RHI_VERTEX_FORMAT_FLOAT2 },
        { .binding = 0, .location = 1, .offset =  8, .format = RHI_VERTEX_FORMAT_FLOAT2 },
        { .binding = 0, .location = 2, .offset = 16, .format = RHI_VERTEX_FORMAT_UNORM4 },
    };

    rhi_color_target_t ct = {
        .format       = RHI_FORMAT_BGRA8_SRGB,    /* matches the swapchain format */
        .blend_enable = true,
        .src_color    = RHI_BLEND_SRC_ALPHA,
        .dst_color    = RHI_BLEND_ONE_MINUS_SRC_A,
        .color_op     = RHI_BLEND_OP_ADD,
        .src_alpha    = RHI_BLEND_ONE,
        .dst_alpha    = RHI_BLEND_ONE_MINUS_SRC_A,
        .alpha_op     = RHI_BLEND_OP_ADD,
    };

    s_f4_pipeline = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vert,
        .frag               = frag,
        .attribs            = { attribs[ 0 ], attribs[ 1 ], attribs[ 2 ] },
        .attrib_count       = 3,
        .vertex_stride      = sizeof( f4_vert_t ),
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { ct },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( f4_push_t ),
        .debug_name         = "f4_sampled",
    } );
    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );
    if ( !rhi_handle_valid( s_f4_pipeline ) )
        return false;

    s_f4_sampler = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_NEAREST,
        .mag_filter = RHI_FILTER_NEAREST,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( !rhi_handle_valid( s_f4_sampler ) )
    {
        rhi()->pipeline_destroy( s_f4_pipeline );
        s_f4_pipeline = ( rhi_pipeline_t ){ 0 };
        return false;
    }
    s_f4_sampler_idx = rhi()->register_sampler( s_f4_sampler );

    /* Unit quad (two triangles), pos == uv, solid white; the texture drives alpha via s.r. */
    f4_vert_t quad[ 6 ] = {
        { 0, 0, 0, 0, 0xFFFFFFFFu }, { 1, 0, 1, 0, 0xFFFFFFFFu }, { 1, 1, 1, 1, 0xFFFFFFFFu },
        { 0, 0, 0, 0, 0xFFFFFFFFu }, { 1, 1, 1, 1, 0xFFFFFFFFu }, { 0, 1, 0, 1, 0xFFFFFFFFu },
    };
    s_f4_quad_vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = sizeof( quad ),
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "f4_quad",
    } );
    if ( !rhi_handle_valid( s_f4_quad_vb ) )
    {
        rhi()->unregister_sampler( s_f4_sampler_idx );
        rhi()->sampler_destroy( s_f4_sampler );
        rhi()->pipeline_destroy( s_f4_pipeline );
        s_f4_sampler = ( rhi_sampler_t ){ 0 };
        s_f4_pipeline = ( rhi_pipeline_t ){ 0 };
        return false;
    }
    rhi()->buffer_write( s_f4_quad_vb, quad, sizeof( quad ), 0 );

    return true;
}

/* Retire one sampled texture: drop its bindless slot, then defer-destroy the image. */
static void
f4_retire_slot( u32 i )
{
    rhi()->unregister_texture( s_f4_sampled[ i ].idx );
    rhi()->texture_destroy( s_f4_sampled[ i ].tex );
    s_f4_sampled[ i ] = ( f4_sampled_t ){ 0 };
}

/* Create one new sampled texture into a free pool slot (no-op when the pool is full). */
static void
f4_spawn( u64 frame )
{
    i32 slot = -1;
    for ( u32 i = 0; i < STRESS_SAMPLED_MAX; ++i )
        if ( !s_f4_sampled[ i ].live ) { slot = (i32)i; break; }
    if ( slot < 0 )
        return;

    rhi_texture_t tex = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = STRESS_SAMPLED_DIM,
        .height       = STRESS_SAMPLED_DIM,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_RGBA8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = "f4_tex",
    } );
    if ( !rhi_handle_valid( tex ) )
        return;

    /* Checkerboard, phase-shifted per spawn so each tile is visibly distinct and animates. */
    static u8 px[ STRESS_SAMPLED_DIM * STRESS_SAMPLED_DIM * 4 ];
    u32 phase = (u32)frame;
    for ( u32 y = 0; y < STRESS_SAMPLED_DIM; ++y )
        for ( u32 x = 0; x < STRESS_SAMPLED_DIM; ++x )
        {
            u8  c = ( ( ( x >> 1 ) ^ ( y >> 1 ) ^ phase ) & 1 ) ? 255 : 40;
            u8* p = &px[ ( y * STRESS_SAMPLED_DIM + x ) * 4 ];
            p[ 0 ] = c; p[ 1 ] = c; p[ 2 ] = c; p[ 3 ] = c;
        }

    if ( !rhi()->upload_texture( tex, px, sizeof( px ), 0, 0 ) )
    {
        rhi()->texture_destroy( tex );
        return;
    }

    u32 idx = rhi()->register_texture( tex );
    if ( idx == 0 )   /* bindless pool exhausted */
    {
        rhi()->texture_destroy( tex );
        return;
    }

    s_f4_sampled[ slot ] = ( f4_sampled_t ){ .tex = tex, .idx = idx, .born = frame, .live = true };
}

/* Retire any sampled texture older than its lifetime.  Runs every frame so the pool drains even
   after F4 is toggled off. */
static void
f4_retire_aged( u64 frame )
{
    for ( u32 i = 0; i < STRESS_SAMPLED_MAX; ++i )
        if ( s_f4_sampled[ i ].live && ( frame - s_f4_sampled[ i ].born ) >= STRESS_SAMPLED_LIFETIME )
            f4_retire_slot( i );
}

/* Retire everything (shutdown, after contexts are destroyed and the GPU is idle). */
static void
f4_retire_all( void )
{
    for ( u32 i = 0; i < STRESS_SAMPLED_MAX; ++i )
        if ( s_f4_sampled[ i ].live )
            f4_retire_slot( i );
}

/* Draw every live sampled texture as a grid quad -- the actual shader read of each upload. */
static void
f4_draw( rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0, .y = 0, .width = (f32)win_w, .height = (f32)win_h, .min_depth = 0, .max_depth = 1 } );
    rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){ .x = 0, .y = 0, .width = win_w, .height = win_h } );
    rhi()->cmd_bind_pipeline( cmd, s_f4_pipeline );
    rhi()->cmd_bind_bindless( cmd );
    rhi()->cmd_bind_vertex_buffer( cmd, s_f4_quad_vb, 0 );

    const i32 cols = 4;
    const i32 rows = ( STRESS_SAMPLED_MAX + cols - 1 ) / cols;
    i32       cell = 0;

    for ( u32 i = 0; i < STRESS_SAMPLED_MAX; ++i )
    {
        if ( !s_f4_sampled[ i ].live )
            continue;

        f32 cw  = (f32)win_w / (f32)cols;
        f32 ch  = (f32)win_h / (f32)rows;
        f32 pad = cw * 0.12f;
        f32 px  = ( cell % cols ) * cw + pad;
        f32 py  = ( cell / cols ) * ch + pad;
        f32 pw  = cw - 2.0f * pad;
        f32 ph  = ch - 2.0f * pad;
        ++cell;

        /* mvp maps the unit quad [0,1]^2 -> pixel rect (px,py,pw,ph) -> Vulkan NDC [-1,+1]. */
        f4_push_t push = { 0 };
        push.mvp[  0 ] = 2.0f * pw / (f32)win_w;
        push.mvp[  5 ] = 2.0f * ph / (f32)win_h;
        push.mvp[ 10 ] = 1.0f;
        push.mvp[ 12 ] = 2.0f * px / (f32)win_w - 1.0f;
        push.mvp[ 13 ] = 2.0f * py / (f32)win_h - 1.0f;
        push.mvp[ 15 ] = 1.0f;
        push.tex_idx   = s_f4_sampled[ i ].idx;
        push.samp_idx  = s_f4_sampler_idx;

        rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );
        rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){ .vertex_count = 6, .instance_count = 1 } );
    }
}

/* Destroy the shared GPU resources (after contexts are gone / GPU idle). */
static void
f4_shutdown( void )
{
    if ( rhi_handle_valid( s_f4_quad_vb ) )  rhi()->buffer_destroy( s_f4_quad_vb );
    if ( s_f4_sampler_idx )                  rhi()->unregister_sampler( s_f4_sampler_idx );
    if ( rhi_handle_valid( s_f4_sampler ) )  rhi()->sampler_destroy( s_f4_sampler );
    if ( rhi_handle_valid( s_f4_pipeline ) ) rhi()->pipeline_destroy( s_f4_pipeline );

    s_f4_quad_vb     = ( rhi_buffer_t ){ 0 };
    s_f4_sampler     = ( rhi_sampler_t ){ 0 };
    s_f4_pipeline    = ( rhi_pipeline_t ){ 0 };
    s_f4_sampler_idx = 0;
}

/*==============================================================================================
    Render one context

    No scene geometry -- a single animated clear is enough to drive the full frame_begin /
    record / frame_end sync path for this context.  The unique per-window phase makes it obvious
    at a glance that every context is independently advancing.  In F4 sampled mode the clear is
    followed by a grid of textured quads (f4_draw) within the same render pass.
==============================================================================================*/

static bool
render_context( const stress_win_t* sw, f64 t, bool draw_sampled )
{
    /* Always call frame_begin, even for a minimized window: that is how this context checks
       into the shared epoch (vk_frame.c).  With multiple contexts the epoch only advances --
       and uploads only flush, garbage only reclaims -- once every live context has checked in.
       Skipping frame_begin on one window starves all of them. */
    rhi_cmd_t cmd = rhi()->frame_begin( sw->ctx );
    if ( !rhi_cmd_valid( cmd ) )
        return false;   /* swapchain not ready (minimized / out-of-date); frame_begin already
                           checked in, and the command buffer was never begun -- so NO frame_end */

    f32 r = 0.5f + 0.5f * (f32)sin( t * 1.1 + sw->phase );
    f32 g = 0.5f + 0.5f * (f32)sin( t * 1.7 + sw->phase + 2.0 );
    f32 b = 0.5f + 0.5f * (f32)sin( t * 2.3 + sw->phase + 4.0 );

    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { r * 0.4f, g * 0.4f, b * 0.4f, 1.0f },
    }, 1, NULL );

    /* F4: sample the uploaded textures with a real draw inside the same pass. */
    if ( draw_sampled )
        f4_draw( cmd, sw->w, sw->h );

    rhi()->cmd_end_rendering( cmd );

    rhi()->frame_end( sw->ctx );
    return true;
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* ------------------------------------------------------------------------------ */
    /* Load modules (no draw, no imgui -- this is a bare RHI stress harness). */

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_vulkan_stress] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    assert( sys() );
    assert( ref() );
    assert( app() );
    assert( core() );
    assert( rhi() );

    core()->log_set_min_level( LOG_LEVEL_TRACE );
    core_log_fn( LOG_LEVEL_DEBUG, "sb_vulkan_stress", "modules loaded" );

    LOG_LINE();

    /* ------------------------------------------------------------------------------ */
    /* Global RHI init (instance + device) -- no window needed yet. */

    if ( !rhi()->init() )
    {
        fprintf( stderr, "[sb_vulkan_stress] rhi->init failed\n" );
        mod_system_exit();
        return 1;
    }

    /* ------------------------------------------------------------------------------ */
    /* Open N windows, one rhi context each.  Stagger their positions so they do not
       fully overlap, and give each a unique color phase. */

    const i32 base_w = 640;
    const i32 base_h = 360;

    for ( u32 i = 0; i < STRESS_WINDOW_COUNT; ++i )
    {
        s_wins[ i ].ctx = RHI_CTX_INVALID;

        char title[ 64 ];
        snprintf( title, sizeof( title ), "sb_vulkan_stress #%u", i );

        i32 x = 60 + (i32)( i % 2 ) * ( base_w + 30 );
        i32 y = 60 + (i32)( i / 2 ) * ( base_h + 50 );

        win_id_t win = app()->window_open( title, x, y, base_w, base_h, APP_WIN_DEFAULT );
        if ( win == APP_WIN_INVALID )
        {
            fprintf( stderr, "[sb_vulkan_stress] window_open #%u failed\n", i );
            continue;
        }

        void* hwnd = app()->window_handle( win );
        i32   ctx  = rhi()->context_create( win, hwnd, base_w, base_h );
        if ( ctx == RHI_CTX_INVALID )
        {
            fprintf( stderr, "[sb_vulkan_stress] context_create #%u failed\n", i );
            app()->window_close( win );
            continue;
        }

        s_wins[ i ] = ( stress_win_t ){
            .win   = win,
            .ctx   = ctx,
            .w     = base_w,
            .h     = base_h,
            .phase = (f32)i * 1.3f,
            .open  = true,
        };
    }

    if ( open_window_count() == 0 )
    {
        fprintf( stderr, "[sb_vulkan_stress] no windows opened\n" );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    /* Build the shared F4 GPU resources (pipeline / sampler / quad).  Optional: if it fails the
       test still runs, F4 just stays unavailable. */
    bool f4_ok = f4_init();
    if ( !f4_ok )
        fprintf( stderr, "[sb_vulkan_stress] f4_init failed; sampled mode (F4) disabled\n" );

    /* ------------------------------------------------------------------------------ */
    /* Run. */

    printf( "[sb_vulkan_stress] running %u windows -- ESC quit, F1 aggressive, F2 stats, F3 ctx-churn, F4 sampled, +/- churn\n",
            open_window_count() );

    u32  churn       = STRESS_CHURN_DEFAULT;   /* assets created per context per frame */
    bool aggressive  = false;                  /* F1: free same-frame to probe deferred-free */
    bool ctx_churn   = false;                  /* F3: periodically destroy+recreate one context */
    bool sample_mode = false;                  /* F4: upload+register+sample textures in a draw */
    u64  frame_no    = 0;

    f64 last_time = sys_tick_seconds();

    while ( app()->pump_events() )
    {
        f64 now_time = sys_tick_seconds();
        f32 dt       = (f32)( now_time - last_time );
        last_time    = now_time;
        UNUSED( dt );

        /* ---- input + window lifecycle ---- */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            switch ( ev.type )
            {
                case APP_EV_WIN_RESIZE:
                {
                    stress_win_t* sw = find_win( ev.win_id );
                    if ( sw && sw->ctx != RHI_CTX_INVALID )
                    {
                        sw->w = ev.data.win_resize.w;
                        sw->h = ev.data.win_resize.h;
                        rhi()->context_resize( sw->ctx, sw->w, sw->h );
                    }
                    break;
                }

                case APP_EV_WIN_CLOSE:
                {
                    stress_win_t* sw = find_win( ev.win_id );
                    if ( sw )
                        close_window( sw );
                    break;
                }

                default:
                    break;
            }
        }

        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            break;

        if ( app()->key_pressed( APP_KEY_F1 ) )
        {
            aggressive = !aggressive;
            printf( "[sb_vulkan_stress] aggressive free %s\n", aggressive ? "ON" : "OFF" );
        }

        if ( app()->key_pressed( APP_KEY_F2 ) )
            printf( "[sb_vulkan_stress] live=%u peak=%u created=%llu freed=%llu churn=%u/ctx\n",
                    s_asset_count, s_peak_live,
                    (unsigned long long)s_total_created,
                    (unsigned long long)s_total_freed, churn );

        if ( app()->key_pressed( APP_KEY_F3 ) )
        {
            ctx_churn = !ctx_churn;
            printf( "[sb_vulkan_stress] context churn %s\n", ctx_churn ? "ON" : "OFF" );
        }

        if ( app()->key_pressed( APP_KEY_F4 ) )
        {
            if ( f4_ok )
            {
                sample_mode = !sample_mode;
                printf( "[sb_vulkan_stress] sampled mode %s\n", sample_mode ? "ON" : "OFF" );
            }
            else
                printf( "[sb_vulkan_stress] sampled mode unavailable (f4_init failed)\n" );
        }

        if ( app()->key_pressed( APP_KEY_NP_ADD ) && churn < STRESS_CHURN_MAX )
            printf( "[sb_vulkan_stress] churn = %u/ctx\n", ++churn );
        if ( app()->key_pressed( APP_KEY_NP_SUB ) && churn > 0 )
            printf( "[sb_vulkan_stress] churn = %u/ctx\n", --churn );

        /* All windows closed by the user -- nothing left to drive. */
        if ( open_window_count() == 0 )
            break;

        /* ---- F3: recycle one context periodically (destroy + recreate) ----
           Done at the top of the frame, before any frame_begin, so the recycled window has a
           valid context for this frame's render.  Round-robins through the windows so over time
           every context gets torn down and rebuilt while its siblings keep rendering. */
        if ( ctx_churn && ( frame_no % STRESS_CTX_CHURN_INTERVAL ) == 0 )
        {
            u32 pick = (u32)( ( frame_no / STRESS_CTX_CHURN_INTERVAL ) % STRESS_WINDOW_COUNT );
            recycle_context( &s_wins[ pick ] );
        }

        /* ---- F4: retire aged sampled textures (always, so the pool drains after F4 is toggled
           off) and spawn a fresh one each frame while sampled mode is on. ---- */
        if ( f4_ok )
        {
            f4_retire_aged( frame_no );
            if ( sample_mode )
                f4_spawn( frame_no );
        }

        /* ---- pump every live context (drives per-context sync + epoch check-in each frame) ----
           Note: minimized windows are NOT skipped here -- frame_begin must run on every context
           so it checks into the epoch.  render_context returns false when the swapchain was not
           ready (minimized), in which case we skip the asset churn for that window but the
           check-in has still happened. */
        for ( u32 i = 0; i < STRESS_WINDOW_COUNT; ++i )
        {
            stress_win_t* sw = &s_wins[ i ];
            if ( !sw->open || sw->ctx == RHI_CTX_INVALID )
                continue;

            if ( !render_context( sw, now_time, sample_mode ) )
                continue;   /* minimized / not ready: checked in, but nothing to draw or churn */

            /* ---- asset churn for this context ---- */
            for ( u32 c = 0; c < churn; ++c )
                if ( !create_asset( frame_no ) )
                    break;   /* ring full or staging exhausted this frame */
        }

        /* In aggressive mode free everything created this pass immediately; otherwise let
           assets age out naturally past the frames-in-flight window. */
        sweep_assets( frame_no, aggressive ? 0 : STRESS_FREE_DELAY );

        ++frame_no;
        sys_sleep_milliseconds( 2 );
    }

    /* ------------------------------------------------------------------------------ */
    /* Shutdown.  Destroy every context first -- each idles its GPU work -- so that the
       remaining transient assets can be freed with no frame still referencing them. */

    printf( "[sb_vulkan_stress] shutting down: live=%u created=%llu freed=%llu peak=%u\n",
            s_asset_count, (unsigned long long)s_total_created,
            (unsigned long long)s_total_freed, s_peak_live );

    for ( u32 i = 0; i < STRESS_WINDOW_COUNT; ++i )
        close_window( &s_wins[ i ] );

    free_all_assets();

    /* Sampled textures + shared F4 resources: contexts are destroyed (GPU idle) above, so the
       deferred destroys queued here are safe to force-drain in rhi()->shutdown(). */
    f4_retire_all();
    f4_shutdown();

    rhi()->shutdown();
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
