/*==============================================================================================

    sandbox/rhi/sb_quad_pull/sb_quad_pull.c -- the bufferless quad-record proof (stage 0 of the
    quad-record renderer).

    Two pipelines draw the same N quads and are measured against each other:

        PULL  no vertex buffer at all.  cmd_draw of 6 * N bare vertices; the vertex stage
              computes quad / corner from SV_VertexID, fetches the 48-byte gui_quad_t record
              from a bindless storage buffer, fetches its style's feather for the expansion
              pad, and expands centre +- (half-extent + pad) itself.
        VB    the control arm: the CPU expands every quad into six 20-byte vertices
              (qp_vert_t layout) and the vertex stage transforms what arrives, exactly
              like today's gui pipeline.

    Both run through the real cook path -- the 'shader' lines on this target produce
    bin/shaders/qp_*.oshd and pipeline_create validates the layouts against their reflection.
    The fragment stage is shared and near-trivial, and the quads are tiny (1-2 px half-extent),
    so the vertex stage dominates and the A/B isolates the pulled-fetch cost.

    Modes:
        sb_quad_pull -bench     unattended A/B sweep over quad counts, prints a table, exits
        sb_quad_pull            interactive:
            ESC   quit
            F1    toggle arm (pull / vb)
            F2    print stats now
            F3    toggle animate (rewrite + re-upload the geometry every frame -- adds the
                  CPU emit cost of each arm: 48 B/quad records vs 6 expanded verts/quad)
            F4    cycle quad count (10k / 40k / 160k / 256k)
            F5    toggle quad size (tiny = VS-bound, large = fill-bound; the arms should
                  converge when fill dominates)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"

/* Types only: gui_quad_t (the record under proof), gui_prim_t (the style record), and
   gui_uv_pack.  Nothing from the gui library links in. */
#include "runtime_service/gui/gui.h"

// clang-format off

/* The VB arm's 20-byte packed vertex -- the layout the gui's retired vertex-buffer pipeline
   used, kept local so the control arm stays measurable against history. */
typedef struct
{
    f32 x, y;     // pixel position
    u32 uv;       // texture UV, two unorm16 (gui_uv_pack)
    u32 abgr;     // packed color
    u32 prim;     // style record index

} qp_vert_t;

/*==============================================================================================
    Tunables
==============================================================================================*/

#define QP_MAX_QUADS    262144u                     /* SSBO 12 MB, VB 30 MB -- both host-visible */
#define QP_STYLES       8u                          /* style table entries (feather varies)      */
#define QP_WIN_W        1280
#define QP_WIN_H        720

#define QP_BENCH_WARMUP 20                          /* frames discarded before timing            */
#define QP_BENCH_FRAMES 200                         /* frames per timed cell                     */
#define QP_BENCH_ANIM   100                         /* frames per animated (rewriting) cell      */

static const u32 k_counts[] = { 10000u, 40000u, 160000u, QP_MAX_QUADS };
#define QP_COUNT_STEPS  ( (u32)( sizeof( k_counts ) / sizeof( k_counts[ 0 ] ) ) )

/* Push constants, mirrored by all three qp_*.hlsl shaders. */
typedef struct
{
    f32 sx, sy;         // pixel -> NDC scale (2/w, 2/h); Vulkan clip space, +y down
    f32 ox, oy;         // pixel -> NDC offset (-1, -1)
    u32 quad_buf;       // bindless buffer slot of the quad records
    u32 quad_base;      // first record of this draw
    u32 style_buf;      // bindless buffer slot of the style records
    u32 style_base;     // first style of this draw

} qp_push_t;

/* The style feathers.  The pull vertex stage fetches style row 2 for its expansion pad; the VB
   arm applies the same pad on the CPU during expansion, so both arms rasterize identical quads. */
static const f32 k_style_feather[ QP_STYLES ] = { 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 2.0f, 2.0f };

/*==============================================================================================
    State
==============================================================================================*/

static gui_quad_t*      s_quads;       /* the authoritative quads, uploaded whole for the pull arm */
static qp_vert_t* s_verts;       /* the VB arm's CPU expansion of the same quads             */

static rhi_pipeline_t   s_pipe_pull;
static rhi_pipeline_t   s_pipe_vb;
static rhi_buffer_t     s_quad_buf;
static u32              s_quad_buf_idx;
static rhi_buffer_t     s_style_buf;
static u32              s_style_buf_idx;
static rhi_buffer_t     s_vb;

static win_id_t         s_win = APP_WIN_INVALID;
static i32              s_ctx = RHI_CTX_INVALID;
static i32              s_w   = QP_WIN_W;
static i32              s_h   = QP_WIN_H;

/*==============================================================================================
    Quad generation + the VB expansion

    Deterministic LCG so every run (and both arms) sees the same layout.  Tiny quads keep the
    proof VS-bound: ~256k quads at 2-4 px cover the window about once, so fill never dominates.
==============================================================================================*/

static u32
qp_rand( u32* state )
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void
gen_quads( u32 count, bool large )
{
    u32 rng = 0xC0FFEEu;

    for ( u32 i = 0; i < count; ++i )
    {
        f32 hw = large ? 6.0f + (f32)( qp_rand( &rng ) % 8u )
                       : 1.0f + (f32)( qp_rand( &rng ) % 2u );
        f32 hh = large ? 6.0f + (f32)( qp_rand( &rng ) % 8u )
                       : 1.0f + (f32)( qp_rand( &rng ) % 2u );

        f32 cx = 16.0f + (f32)( qp_rand( &rng ) % (u32)( s_w - 32 ) );
        f32 cy = 16.0f + (f32)( qp_rand( &rng ) % (u32)( s_h - 32 ) );

        u32 abgr = 0xFF000000u
                 | ( ( 0x40u + ( qp_rand( &rng ) & 0xBFu ) ) << 16 )
                 | ( ( 0x40u + ( qp_rand( &rng ) & 0xBFu ) ) << 8 )
                 |   ( 0x40u + ( qp_rand( &rng ) & 0xBFu ) );

        s_quads[ i ] = ( gui_quad_t ){
            .cx    = cx,
            .cy    = cy,
            .hw    = hw,
            .hh    = hh,
            .uv0   = gui_uv_pack( 0.0f, 0.0f ),
            .uv1   = gui_uv_pack( 1.0f, 1.0f ),
            .abgr  = abgr,
            .style = i % QP_STYLES,
        };
    }
}

/* A cheap per-frame wobble on every centre -- the "animate" workload.  It mutates the
   authoritative quads, so each arm's rebuild cost is measured from the same source. */
static void
jitter_quads( u32 count, u64 frame )
{
    f32 t = (f32)( frame % 628u ) * 0.01f;
    for ( u32 i = 0; i < count; ++i )
    {
        s_quads[ i ].cx += sinf( t + (f32)( i & 63u ) ) * 0.25f;
        s_quads[ i ].cy += cosf( t + (f32)( i & 31u ) ) * 0.25f;
    }
}

/* The VB arm's whole vertex cost: six positioned corners per quad, feather pad applied on the
   CPU -- what the tessellator does today, and what the pull vertex stage does on the GPU. */
static void
expand_vb( u32 count )
{
    static const f32 k_cx[ 6 ] = { 0, 1, 1, 0, 1, 0 };
    static const f32 k_cy[ 6 ] = { 0, 0, 1, 0, 1, 1 };

    for ( u32 i = 0; i < count; ++i )
    {
        const gui_quad_t* q   = &s_quads[ i ];
        f32               pad = k_style_feather[ q->style ] + 1.0f;
        f32               x0  = q->cx - ( q->hw + pad );
        f32               y0  = q->cy - ( q->hh + pad );
        f32               x1  = q->cx + ( q->hw + pad );
        f32               y1  = q->cy + ( q->hh + pad );

        qp_vert_t* v = &s_verts[ i * 6u ];
        for ( u32 c = 0; c < 6u; ++c )
        {
            v[ c ] = ( qp_vert_t ){
                .x    = x0 + ( x1 - x0 ) * k_cx[ c ],
                .y    = y0 + ( y1 - y0 ) * k_cy[ c ],
                .uv   = gui_uv_pack( k_cx[ c ], k_cy[ c ] ),
                .abgr = q->abgr,
                .prim = q->style,
            };
        }
    }
}

static void
upload_quads( u32 count )
{
    rhi()->buffer_write( s_quad_buf, s_quads, count * sizeof( gui_quad_t ), 0 );
}

static void
upload_vb( u32 count )
{
    rhi()->buffer_write( s_vb, s_verts, count * 6u * sizeof( qp_vert_t ), 0 );
}

/*==============================================================================================
    GPU setup
==============================================================================================*/

static rhi_shader_t
load_oshd( const char* stem, const char* dbg )
{
    char dir[ 512 ];
    sys_exe_dir( dir, (int)sizeof( dir ) );

    char path[ 640 ];
    snprintf( path, sizeof( path ), "%s/shaders/%s.oshd", dir, stem );

    rhi_shader_t sh = rhi()->shader_load_oshd( path, dbg );
    if ( !rhi_handle_valid( sh ) )
        fprintf( stderr, "[sb_quad_pull] shader not found: %s (build sb_quad_pull to cook it)\n", path );
    return sh;
}

static bool
gpu_init( void )
{
    rhi_shader_t vs_pull = load_oshd( "qp_pull.vs", "qp_pull_vert" );
    rhi_shader_t vs_vb   = load_oshd( "qp_vb.vs",   "qp_vb_vert" );
    rhi_shader_t ps      = load_oshd( "qp.ps",      "qp_frag" );
    if ( !rhi_handle_valid( vs_pull ) || !rhi_handle_valid( vs_vb ) || !rhi_handle_valid( ps ) )
        return false;

    rhi_color_target_t ct = {
        .format       = RHI_FORMAT_BGRA8_SRGB,
        .blend_enable = true,
        .src_color    = RHI_BLEND_SRC_ALPHA,
        .dst_color    = RHI_BLEND_ONE_MINUS_SRC_A,
        .color_op     = RHI_BLEND_OP_ADD,
        .src_alpha    = RHI_BLEND_ONE,
        .dst_alpha    = RHI_BLEND_ONE_MINUS_SRC_A,
        .alpha_op     = RHI_BLEND_OP_ADD,
    };

    /* The pull pipeline is the point of the proof: no attributes, vertex_stride 0, so the RHI
       creates it with no vertex binding at all and the stage runs on SV_VertexID alone. */
    s_pipe_pull = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vs_pull,
        .frag               = ps,
        .attrib_count       = 0,
        .vertex_stride      = 0,
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { ct },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( qp_push_t ),
        .debug_name         = "qp_pull",
    } );

    /* The control arm: today's 20-byte gui vertex, literal offsets pinned to the struct. */
    ORB_STATIC_ASSERT( sizeof( qp_vert_t ) == 20, "the VB arm states these offsets as literals" );

    s_pipe_vb = rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vs_vb,
        .frag               = ps,
        .attribs            = {
            { .binding = 0, .location = 0, .offset =  0, .format = RHI_VERTEX_FORMAT_FLOAT2    },
            { .binding = 0, .location = 1, .offset =  8, .format = RHI_VERTEX_FORMAT_UNORM16X2 },
            { .binding = 0, .location = 2, .offset = 12, .format = RHI_VERTEX_FORMAT_UNORM8X4  },
            { .binding = 0, .location = 3, .offset = 16, .format = RHI_VERTEX_FORMAT_UINT      },
        },
        .attrib_count       = 4,
        .vertex_stride      = sizeof( qp_vert_t ),
        .cull               = RHI_CULL_NONE,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { ct },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( qp_push_t ),
        .debug_name         = "qp_vb",
    } );

    rhi()->shader_destroy( ps );
    rhi()->shader_destroy( vs_vb );
    rhi()->shader_destroy( vs_pull );

    if ( !rhi_handle_valid( s_pipe_pull ) || !rhi_handle_valid( s_pipe_vb ) )
        return false;

    /* The quad-record table the pull stage reads. */
    s_quad_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = QP_MAX_QUADS * sizeof( gui_quad_t ),
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "qp_quads",
    } );
    if ( rhi_handle_valid( s_quad_buf ) )
        s_quad_buf_idx = rhi()->register_buffer( s_quad_buf );
    if ( s_quad_buf_idx == 0 )
        return false;

    /* The style table: gui_prim_t records; the pull stage fetches row 2 (feather) per vertex
       for the expansion pad. */
    s_style_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = QP_STYLES * sizeof( gui_prim_t ),
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "qp_styles",
    } );
    if ( rhi_handle_valid( s_style_buf ) )
        s_style_buf_idx = rhi()->register_buffer( s_style_buf );
    if ( s_style_buf_idx == 0 )
        return false;

    gui_prim_t styles[ QP_STYLES ] = { 0 };
    for ( u32 i = 0; i < QP_STYLES; ++i )
        styles[ i ].feather = k_style_feather[ i ];
    rhi()->buffer_write( s_style_buf, styles, sizeof( styles ), 0 );

    /* The control arm's vertex buffer. */
    s_vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = QP_MAX_QUADS * 6u * sizeof( qp_vert_t ),
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "qp_vb",
    } );
    if ( !rhi_handle_valid( s_vb ) )
        return false;

    return true;
}

static void
gpu_shutdown( void )
{
    if ( rhi_handle_valid( s_vb ) )         rhi()->buffer_destroy( s_vb );
    if ( s_style_buf_idx )                  rhi()->unregister_buffer( s_style_buf_idx );
    if ( rhi_handle_valid( s_style_buf ) )  rhi()->buffer_destroy( s_style_buf );
    if ( s_quad_buf_idx )                   rhi()->unregister_buffer( s_quad_buf_idx );
    if ( rhi_handle_valid( s_quad_buf ) )   rhi()->buffer_destroy( s_quad_buf );
    if ( rhi_handle_valid( s_pipe_vb ) )    rhi()->pipeline_destroy( s_pipe_vb );
    if ( rhi_handle_valid( s_pipe_pull ) )  rhi()->pipeline_destroy( s_pipe_pull );
}

/*==============================================================================================
    One frame
==============================================================================================*/

static bool
draw_frame( bool pull, u32 count )
{
    rhi_cmd_t cmd = rhi()->frame_begin( s_ctx );
    if ( !rhi_cmd_valid( cmd ) )
        return false;   /* swapchain not ready (minimized / out-of-date); no frame_end */

    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { 0.06f, 0.06f, 0.08f, 1.0f },
    }, 1, NULL );

    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0, .y = 0, .width = (f32)s_w, .height = (f32)s_h, .min_depth = 0, .max_depth = 1 } );
    rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){ .x = 0, .y = 0, .width = s_w, .height = s_h } );

    rhi()->cmd_bind_pipeline( cmd, pull ? s_pipe_pull : s_pipe_vb );
    rhi()->cmd_bind_bindless( cmd );
    if ( !pull )
        rhi()->cmd_bind_vertex_buffer( cmd, s_vb, 0 );

    qp_push_t push = {
        .sx        = 2.0f / (f32)s_w,
        .sy        = 2.0f / (f32)s_h,
        .ox        = -1.0f,
        .oy        = -1.0f,
        .quad_buf  = s_quad_buf_idx,
        .quad_base = 0,
        .style_buf = s_style_buf_idx,
        .style_base = 0,
    };
    rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

    rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){ .vertex_count = count * 6u, .instance_count = 1 } );

    rhi()->cmd_end_rendering( cmd );
    rhi()->frame_end( s_ctx );
    return true;
}

/* Drain window events; false once the window is gone or ESC was hit. */
static bool
pump( void )
{
    if ( !app()->pump_events() )
        return false;

    app_event_t ev;
    while ( app()->next_event( &ev ) )
    {
        if ( ev.type == APP_EV_WIN_RESIZE && ev.win_id == s_win )
        {
            s_w = ev.data.win_resize.w;
            s_h = ev.data.win_resize.h;
            rhi()->context_resize( s_ctx, s_w, s_h );
        }
        if ( ev.type == APP_EV_WIN_CLOSE && ev.win_id == s_win )
            return false;
    }
    return !app()->key_pressed( APP_KEY_ESCAPE );
}

/*==============================================================================================
    The bench: an unattended A/B sweep.  Static cells isolate the GPU vertex stage; the animated
    cells add each arm's per-frame CPU emit (record rewrite vs 6-vertex expansion) + upload.
==============================================================================================*/

typedef struct { f64 ms; f64 build_ms; } bench_cell_t;

/* One timed cell.  frames of warmup, then frames timed; animate rewrites + uploads per frame. */
static bench_cell_t
bench_cell( bool pull, u32 count, bool animate, int frames )
{
    bench_cell_t out         = { 0 };
    f64          t0          = 0.0;
    f64          build_total = 0.0;
    int          timed       = 0;
    u64          frame       = 0;

    for ( int i = -QP_BENCH_WARMUP; i < frames; ++i )
    {
        if ( !pump() )
            break;

        if ( i == 0 )
            t0 = sys_tick_seconds();   /* warmup done -- start the clock */

        if ( animate )
        {
            f64 b0 = sys_tick_seconds();
            jitter_quads( count, frame++ );
            if ( pull )
            {
                upload_quads( count );
            }
            else
            {
                expand_vb( count );
                upload_vb( count );
            }
            if ( i >= 0 )
                build_total += sys_tick_seconds() - b0;
        }

        draw_frame( pull, count );
        if ( i >= 0 )
            ++timed;
    }

    if ( timed > 0 )
    {
        out.ms       = ( sys_tick_seconds() - t0 ) * 1000.0 / (f64)timed;
        out.build_ms = build_total * 1000.0 / (f64)timed;
    }
    return out;
}

static void
bench_run( void )
{
    printf( "[sb_quad_pull] bench: %dx%d, tiny quads, %d timed frames per cell\n",
            s_w, s_h, QP_BENCH_FRAMES );
    printf( "  %8s  %10s %10s  %10s %10s  %8s\n",
            "quads", "pull ms", "pull fps", "vb ms", "vb fps", "pull/vb" );

    for ( u32 c = 0; c < QP_COUNT_STEPS; ++c )
    {
        u32 count = k_counts[ c ];

        gen_quads( count, false );
        upload_quads( count );
        expand_vb( count );
        upload_vb( count );

        bench_cell_t pull = bench_cell( true,  count, false, QP_BENCH_FRAMES );
        bench_cell_t vb   = bench_cell( false, count, false, QP_BENCH_FRAMES );

        printf( "  %8u  %10.3f %10.0f  %10.3f %10.0f  %8.2f\n",
                count, pull.ms, 1000.0 / pull.ms, vb.ms, 1000.0 / vb.ms, pull.ms / vb.ms );
    }

    /* The animated pair at the top count: the per-frame CPU emit + upload each arm pays. */
    u32 count = k_counts[ QP_COUNT_STEPS - 1 ];
    gen_quads( count, false );
    upload_quads( count );
    expand_vb( count );
    upload_vb( count );

    bench_cell_t pull = bench_cell( true,  count, true, QP_BENCH_ANIM );
    bench_cell_t vb   = bench_cell( false, count, true, QP_BENCH_ANIM );

    printf( "  animated %u: pull %.3f ms/frame (build %.3f ms cpu), "
            "vb %.3f ms/frame (build %.3f ms cpu)\n",
            count, pull.ms, pull.build_ms, vb.ms, vb.build_ms );
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    bool bench      = false;
    int  auto_frames = 0;   /* -frames N: run N interactive frames then exit (smoke testing) */

    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-bench" ) == 0 )
            bench = true;
        else if ( strcmp( argv[ i ], "-frames" ) == 0 && i + 1 < argc )
            auto_frames = atoi( argv[ ++i ] );
    }

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_quad_pull] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_WARN );

    if ( !rhi()->init() )
    {
        fprintf( stderr, "[sb_quad_pull] rhi->init failed\n" );
        mod_system_exit();
        return 1;
    }

    s_win = app()->window_open( "sb_quad_pull", 80, 80, QP_WIN_W, QP_WIN_H, APP_WIN_DEFAULT );
    if ( s_win == APP_WIN_INVALID )
    {
        fprintf( stderr, "[sb_quad_pull] window_open failed\n" );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    s_ctx = rhi()->context_open( s_win );
    if ( s_ctx == RHI_CTX_INVALID )
    {
        fprintf( stderr, "[sb_quad_pull] context_open failed\n" );
        app()->window_close( s_win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    s_quads = malloc( QP_MAX_QUADS * sizeof( gui_quad_t ) );
    s_verts = malloc( QP_MAX_QUADS * 6u * sizeof( qp_vert_t ) );

    int rc = 1;
    if ( s_quads && s_verts && gpu_init() )
    {
        rc = 0;

        if ( bench )
        {
            bench_run();
        }
        else
        {
            bool pull      = true;
            bool animate   = false;
            bool large     = false;
            u32  count_ix  = 0;
            u32  count     = k_counts[ count_ix ];
            u64  frame     = 0;

            gen_quads( count, large );
            upload_quads( count );
            expand_vb( count );
            upload_vb( count );

            printf( "[sb_quad_pull] interactive: ESC quit, F1 arm, F2 stats, F3 animate, "
                    "F4 count, F5 size -- pull, %u quads\n", count );

            f64 sec_t0     = sys_tick_seconds();
            u64 sec_frames = 0;
            f64 sec_build  = 0.0;

            while ( pump() )
            {
                bool regen = false;

                if ( app()->key_pressed( APP_KEY_F1 ) )
                    printf( "[sb_quad_pull] arm: %s\n", ( pull = !pull ) ? "pull" : "vb" );
                if ( app()->key_pressed( APP_KEY_F3 ) )
                    printf( "[sb_quad_pull] animate %s\n", ( animate = !animate ) ? "ON" : "OFF" );
                if ( app()->key_pressed( APP_KEY_F4 ) )
                {
                    count_ix = ( count_ix + 1 ) % QP_COUNT_STEPS;
                    count    = k_counts[ count_ix ];
                    regen    = true;
                    printf( "[sb_quad_pull] count: %u\n", count );
                }
                if ( app()->key_pressed( APP_KEY_F5 ) )
                {
                    regen = true;
                    printf( "[sb_quad_pull] quads: %s\n", ( large = !large ) ? "large (fill-bound)"
                                                                             : "tiny (VS-bound)" );
                }

                if ( regen )
                {
                    gen_quads( count, large );
                    upload_quads( count );
                    expand_vb( count );
                    upload_vb( count );
                }

                if ( animate )
                {
                    f64 b0 = sys_tick_seconds();
                    jitter_quads( count, frame );
                    if ( pull )
                    {
                        upload_quads( count );
                    }
                    else
                    {
                        expand_vb( count );
                        upload_vb( count );
                    }
                    sec_build += sys_tick_seconds() - b0;
                }

                draw_frame( pull, count );
                ++frame;
                ++sec_frames;

                f64 now     = sys_tick_seconds();
                f64 elapsed = now - sec_t0;
                if ( app()->key_pressed( APP_KEY_F2 ) || elapsed >= 2.0 )
                {
                    printf( "[sb_quad_pull] %s %u quads: %.0f fps (%.3f ms/frame%s)\n",
                            pull ? "pull" : "vb", count,
                            (f64)sec_frames / elapsed,
                            elapsed * 1000.0 / (f64)sec_frames,
                            animate ? ", cpu build incl." : "" );
                    if ( animate )
                        printf( "[sb_quad_pull]   cpu build: %.3f ms/frame\n",
                                sec_build * 1000.0 / (f64)sec_frames );
                    sec_t0     = now;
                    sec_frames = 0;
                    sec_build  = 0.0;
                }

                if ( auto_frames > 0 && frame >= (u64)auto_frames )
                    break;
            }
        }
    }

    if ( s_ctx != RHI_CTX_INVALID )
        rhi()->context_destroy( s_ctx );   /* idles this context's GPU work */
    app()->window_close( s_win );

    gpu_shutdown();
    rhi()->shutdown();
    mod_system_exit();

    free( s_verts );
    free( s_quads );
    return rc;
}

/*============================================================================================*/
// clang-format on
