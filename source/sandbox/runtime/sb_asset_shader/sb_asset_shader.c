/*==============================================================================================

    sandbox/runtime/sb_asset_shader/sb_asset_shader.c -- shader asset pipeline proof (shader system Phase 5).

    The whole cook -> acquire -> render -> hot-reload chain in one run:

      1. Spawns asset_tool to cook the sb_tri HLSL pair (source/tools/shader_tool/test/) into
         bin/ -- asset_tool derives the dxc profile from the .vs/.ps stage tag and forwards to
         shader_tool, which bakes SPIR-V + reflection into an .oshd container.
      2. Mounts the repo root on core/fs and acquire()s both .oshd files by virtual path --
         the asset service's built-in "shader" type parses them through
         rhi()->shader_load_oshd_memory behind the ids.
      3. Builds a pipeline from the ACQUIRED shader handles with an empty desc (attribs and
         push_const_size 0), so the RHI derives everything from baked reflection, and renders
         the push-constant-tinted triangle.
      4. Polls asset()->refresh() every frame.  Once mid-run it re-cooks the pixel shader to
         force a reload, then re-get()s the resources, compares the cached layout_hash pair
         (equal = safe swap, different = ABI break), and rebuilds the pipeline from the new
         handles either way -- pipeline_create re-validates against the new reflection.

    Run from the repo root.  Optional numeric arg = auto-quit after N frames (headless smoke).
    While running interactively, re-cook either shader by hand to hot-reload it; ESC quits.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/asset/asset_host.h"
#include "runtime_service/asset/loaders/asset_shader.h"

// clang-format off

#define VS_HLSL   "source\\tools\\shader_tool\\test\\sb_tri.vs.hlsl"
#define PS_HLSL   "source\\tools\\shader_tool\\test\\sb_tri.ps.hlsl"
#define VS_OSHD   "bin/sb_asset_tri.vs.oshd"     /* vpath form; cook uses backslashes */
#define PS_OSHD   "bin/sb_asset_tri.ps.oshd"

/* cook_via_asset_tool -- spawn asset_tool for one .hlsl -> .oshd job.  This is the Phase 5
   dispatch proof: asset_tool picks the profile from the stage tag and forwards to shader_tool. */
static bool
cook_via_asset_tool( const char* src, const char* dst )
{
    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    char dst_os[ 512 ];
    snprintf( dst_os, sizeof( dst_os ), "%s", dst );
    for ( char* p = dst_os; *p; ++p )
        if ( *p == '/' )
            *p = '\\';

    char cmd[ 1536 ];
    snprintf( cmd, sizeof( cmd ), "\"%s\\asset_tool.exe\" cook \"%s\" \"%s\"", exe_dir, src, dst_os );

    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) || res.exit_code != 0 )
    {
        fprintf( stderr, "[sb_asset_shader] asset_tool cook failed for %s (build asset_tool + "
                         "shader_tool, run from the repo root)\n", src );
        return false;
    }
    return true;
}

/* build_pipeline -- pipeline from the two acquired shader assets.  The desc leaves vertex
   input and push constants empty, so the RHI derives both from the baked reflection. */
static rhi_pipeline_t
build_pipeline( const asset_shader_t* vs, const asset_shader_t* ps )
{
    return rhi()->pipeline_create( &( rhi_pipeline_desc_t ){
        .vert               = vs->shader,
        .frag               = ps->shader,
        .cull               = RHI_CULL_NONE,
        .color_targets      = { { .format = RHI_FORMAT_BGRA8_SRGB } },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .debug_name         = "asset_tri_pipeline",
    } );
}

int
main( int argc, char** argv )
{
    int max_frames = ( argc > 1 ) ? atoi( argv[ 1 ] ) : 0;

    /* ---- Cook (Track 1 proof: asset_tool dispatches .hlsl by stage tag) ---- */
    if ( !cook_via_asset_tool( VS_HLSL, VS_OSHD ) || !cook_via_asset_tool( PS_HLSL, PS_OSHD ) )
        return 1;

    /* ---- Modules ---- */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( asset );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_asset_shader] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    /* ---- RHI + window ---- */
    if ( !rhi()->init() )
    {
        mod_system_exit();
        return 1;
    }

    win_id_t win = app()->window_open( "sb_asset_shader", 0, 0, 1024, 768, APP_WIN_DEFAULT );
    i32      ctx = ( win != APP_WIN_INVALID ) ? rhi()->context_open( win ) : RHI_CTX_INVALID;
    if ( ctx == RHI_CTX_INVALID )
    {
        if ( win != APP_WIN_INVALID )
            app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    /* ---- Acquire the shader assets (Track 2 proof: .oshd type behind asset ids) ---- */
    core()->fs_mount( "", "", 0 );    /* repo root (CWD) served verbatim */

    rhi_pipeline_t  pipeline = { 0 };    /* declared before any goto so shutdown can test it */
    asset_id_t      vs_id = asset()->acquire( VS_OSHD );
    asset_id_t      ps_id = asset()->acquire( PS_OSHD );
    asset_shader_t* vs    = ( asset_shader_t* )asset()->get( vs_id );
    asset_shader_t* ps    = ( asset_shader_t* )asset()->get( ps_id );
    if ( !vs || !ps )
    {
        fprintf( stderr, "[sb_asset_shader] shader asset load failed (vs state=%d, ps state=%d)\n",
                 asset()->state( vs_id ), asset()->state( ps_id ) );
        goto shutdown;
    }

    printf( "[sb_asset_shader] acquired %s (hash %016llx) + %s (hash %016llx)\n",
            VS_OSHD, ( unsigned long long )vs->layout_hash,
            PS_OSHD, ( unsigned long long )ps->layout_hash );
    fflush( stdout );

    /* Cache the reload gate: the hash pair the current pipeline was built against. */
    u64 vs_hash = vs->layout_hash;
    u64 ps_hash = ps->layout_hash;

    pipeline = build_pipeline( vs, ps );
    if ( !rhi_handle_valid( pipeline ) )
    {
        fprintf( stderr, "[sb_asset_shader] pipeline_create failed\n" );
        goto shutdown;
    }

    printf( "[sb_asset_shader] running -- re-cook a shader to hot-reload; ESC to quit\n" );
    fflush( stdout );

    /* ---- Render loop ---- */
    {
    int frames      = 0;
    int recook_at   = max_frames ? max_frames / 2 : 120;   /* one automatic mid-run reload */
    bool recooked   = false;

    while ( app()->pump_events() )
    {
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            rhi()->event( &ev );
            if ( ev.type == APP_EV_WIN_CLOSE )
                goto shutdown;
        }
        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            goto shutdown;
        if ( app()->window_is_minimized( win ) )
            continue;

        /* Mid-run re-cook: same source, new mtime -- refresh() below picks it up.  This is the
           automated hot-reload proof; interactively you can re-cook by hand at any time. */
        if ( !recooked && frames == recook_at )
        {
            recooked = true;
            printf( "[sb_asset_shader] re-cooking %s to trigger a hot reload...\n", PS_OSHD );
            fflush( stdout );
            cook_via_asset_tool( PS_HLSL, PS_OSHD );
        }

        /* Hot-reload poll (Track 3 proof).  A reload re-runs the loader behind the same ids
           but mints NEW rhi_shader_t handles, so the pipeline must be rebuilt.  The cached
           layout_hash pair is the gate: equal = pure code swap; different = ABI break (a
           hand-filled desc might now be stale -- ours is empty, and pipeline_create
           re-validates against the new reflection either way). */
        if ( asset()->refresh() > 0 )
        {
            vs = ( asset_shader_t* )asset()->get( vs_id );
            ps = ( asset_shader_t* )asset()->get( ps_id );
            if ( vs && ps )
            {
                bool abi_break = vs->layout_hash != vs_hash || ps->layout_hash != ps_hash;
                printf( "[sb_asset_shader] hot-reload: layout hash %s -- rebuilding pipeline\n",
                        abi_break ? "CHANGED (ABI break: hand-written CPU structs may be stale)"
                                  : "unchanged (safe swap)" );
                fflush( stdout );

                rhi_pipeline_t next = build_pipeline( vs, ps );
                if ( rhi_handle_valid( next ) )
                {
                    rhi()->pipeline_destroy( pipeline );
                    pipeline = next;
                    vs_hash  = vs->layout_hash;
                    ps_hash  = ps->layout_hash;
                }
                else
                    printf( "[sb_asset_shader] pipeline rebuild failed -- keeping the old one\n" );
            }
            else
                printf( "[sb_asset_shader] reload left a shader FAILED -- keeping the old pipeline\n" );
        }

        i32 w = 0, h = 0;
        app()->window_get_size( win, &w, &h );

        rhi_cmd_t cmd = rhi()->frame_begin( ctx );
        if ( !rhi_cmd_valid( cmd ) )
            continue;

        rhi()->cmd_bind_bindless( cmd );

        rhi_color_attachment_t color_att = {
            .texture  = { .id = RHI_SWAPCHAIN_COLOR },
            .load_op  = RHI_LOAD_OP_CLEAR,
            .store_op = RHI_STORE_OP_STORE,
            .clear    = { 0.05f, 0.05f, 0.12f, 1.0f },
        };
        rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );
        rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){ 0, 0, ( f32 )w, ( f32 )h, 0.0f, 1.0f } );
        rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){ 0, 0, w, h } );
        rhi()->cmd_bind_pipeline( cmd, pipeline );

        /* sb_tri.ps reads a float4 tint push constant; pc_size comes from reflection. */
        if ( ps->pc_size > 0 )
        {
            f32 tint[ 4 ] = { 0.9f, 0.6f, 0.1f, 1.0f };    /* amber: distinct from sb_vulkan's blue */
            rhi()->cmd_push_constants( cmd, tint, sizeof( tint ), 0 );
        }

        rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){ .vertex_count = 3, .instance_count = 1 } );
        rhi()->cmd_end_rendering( cmd );
        rhi()->frame_end( ctx );

        if ( max_frames && ++frames >= max_frames )
        {
            printf( "[sb_asset_shader] rendered %d frames -- exiting (headless smoke)\n", frames );
            goto shutdown;
        }
        if ( !max_frames )
            ++frames;
    }
    }

shutdown:
    if ( rhi_handle_valid( pipeline ) )
        rhi()->pipeline_destroy( pipeline );
    asset()->release( ps_id );    /* unloads -> shader_destroy behind the ids */
    asset()->release( vs_id );
    rhi()->context_destroy( ctx );
    rhi()->shutdown();
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
