/*==============================================================================================

    sandbox/vulkan/sb_vulkan.c -- Vulkan RHI bring-up test.

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline.
    Currently uses sb_vk_boot to render a hardcoded triangle until the window is closed
    or ESC is pressed.  No runtime host, no module hot-reload.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/imgui/imgui_host.h"
#include "sb_vulkan_boot.h"
#include "sb_vulkan_imgui.h"

// clang-format off
/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* Load modules. */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( draw ); 
    mod_static( imgui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_vulkan] mod_init_all failed: %s\n", mod_last_error() );
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
    assert( draw() );
    assert( imgui() );

    core()->log_set_min_level( LOG_LEVEL_TRACE );
    core_log_fn( LOG_LEVEL_DEBUG, "sb_vulkan", "debug log: modules loaded successfully" );

    LOG_LINE();

    /* ------------------------------------------------------------------------------ */
    /* Setup Window + RHI */

    /* Open window. */
    win_id_t win = app()->window_open( "sb_vulkan", 0, 0, 1280, 720, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID ) {
         mod_system_exit();
         return 1;
    }

    /* Global RHI init (instance + device). */
    if ( !rhi()->init() ) {
         app()->window_close( win );
         mod_system_exit();
         return 1;
    }

    /* Per-window render context. */
    void* hwnd = app()->window_handle( win );
    i32  ctx   = rhi()->context_create( win, hwnd, 1280, 720 );
    if ( ctx == RHI_CTX_INVALID ) {
         rhi()->shutdown();
         app()->window_close( win );
         mod_system_exit();
         return 1;
    }

    /* ------------------------------------------------------------------------------ */
    /* Setup Resources */

    const bool b_use_boot = false;  // skip bootstrap triangle pipeline

    /* Initialize draw GPU resources (buffers + pipelines) now that the device is live. */
    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_vulkan] draw->init failed\n" );
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    sb_vk_boot_t boot = { 0 };
    if ( b_use_boot )
    {
        if ( !sb_vk_boot_create( &boot ) )
        {
            draw()->shutdown();
            rhi()->context_destroy( ctx );
            rhi()->shutdown();
            app()->window_close( win );
            mod_system_exit();
            return 1;
        }
    }

    /* Initialize imgui GPU resources and load TrueType font atlas. */
    if ( !imgui()->init() )
    {
        fprintf( stderr, "[sb_vulkan] imgui->init failed\n" );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }
    
    //imgui()->load_font( "fonts/jetbrains_regular_24.orb_font" );
    imgui()->load_font( "fonts/jetbrains_bold_24.orb_font" );
    
    imgui()->print_mem_stats();

    /* ------------------------------------------------------------------------------ */
    /* Start render loop. */

    printf( "[sb_vulkan] running -- ESC to quit\n" );
    printf( "[sb_vulkan] imgui demos: 1-9 select, +/- step, F1-F4 debug overlay, NP. font scale\n" );

    /* Track window size for viewport/scissor updates. */
    i32 win_w = 1280;
    i32 win_h = 720;

    /* Active imgui demo index (see sb_vulkan_imgui.c); switched live with the keys below. */
    int active_demo = 0;

    /* Main loop. */
    f64 last_time = sys_tick_seconds();
    while ( app()->pump_events() )
    {
        /* Real frame delta (seconds) -- drives imgui animation + double-click timing. */
        f64 now_time = sys_tick_seconds();
        f32 dt       = (f32)( now_time - last_time );
        last_time    = now_time;

        /* Drain the app event ring once: imgui consumes the input events it cares
           about (text + scroll); the host handles the rest (resize). */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            if ( imgui()->event( &ev ) )
                continue;

            switch ( ev.type )
            {
                case APP_EV_WIN_RESIZE:
                    win_w = ev.data.win_resize.w;
                    win_h = ev.data.win_resize.h;
                    rhi()->context_resize( ctx, win_w, win_h );
                    break;

                default:
                    break;
            }
        }

        if ( app()->key_pressed( APP_KEY_F12 ) )
            break;

        /* ------------------------------------------------------------------------------ */
        /* imgui demo selection.
           Number keys 1-9 jump straight to a demo; +/- step back and forth through the
           table (with wrap).  The active demo is drawn below, inside the imgui frame. */

        const int demo_count = sb_imgui_demo_count();

        for ( int i = 0; i < demo_count && i < 9; ++i )
            if ( app()->key_pressed( (app_key_t)( APP_KEY_1 + i ) ) )
                active_demo = i;

        if ( app()->key_pressed( APP_KEY_NP_ADD ) )
            active_demo = ( active_demo + 1 ) % demo_count;
        if ( app()->key_pressed( APP_KEY_NP_SUB ) )
            active_demo = ( active_demo + demo_count - 1 ) % demo_count;

        /* Bitmap font scale cycle (1x/2x/3x) stays handy on the numpad dot. */
        if ( app()->key_pressed( APP_KEY_NP_DOT ) )
        {
            static int bmp_scale = 1;
            bmp_scale = bmp_scale == 1 ? 2 : ( bmp_scale == 2 ? 3 : 1 );
            imgui()->set_bmp_scale( bmp_scale );
        }

        /* Debug overlay layers (Debug build only): toggle each with the F1-F4 keys.
           F1 window frames   F2 widget interaction rects   F3 resize bands   F4 clip rects. */
        {
            const struct { app_key_t key; u32 bit; } dbg_keys[] = {
                { APP_KEY_F1, IMGUI_DBG_WINDOW   },
                { APP_KEY_F2, IMGUI_DBG_INTERACT },
                { APP_KEY_F3, IMGUI_DBG_RESIZE   },
                { APP_KEY_F4, IMGUI_DBG_CLIP     },
            };
            for ( u32 i = 0; i < 4; ++i )
                if ( app()->key_pressed( dbg_keys[ i ].key ) )
                    imgui()->debug_set_layers( imgui()->debug_get_layers() ^ dbg_keys[ i ].bit );
        }

        /* ------------------------------------------------------------------------------ */
        /* Render frame -- skip entirely while minimized to avoid 0x0 swapchain churn. */

        if ( !app()->window_is_minimized( win ))
        {
            /* valid command recording handle on success */
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                if ( b_use_boot )
                {
                    sb_vk_boot_render( &boot, cmd, win_w, win_h );
                }
                else
                {
                    /* No scene this frame -- clear the swapchain so imgui's LOAD pass
                       composites over a fresh background instead of last frame's pixels
                       (otherwise dragging a window smears: hall-of-mirrors). */
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );
                }

                if ( 0 )
                {
                    /* 2D draw pass -- positions in pixel space (0,0 = top-left). */
                    const f32 bg[ 4 ] = { 0.08f, 0.08f, 0.12f, 1.0f };
                    draw()->begin_pass( cmd, win_w, win_h, bg );
                    const f32 white[ 4 ] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    draw()->rect( win_w * 0.5f, win_h * 0.5f, 200.0f, 100.0f, white );
                    const f32 red[ 4 ] = { 0.9f, 0.2f, 0.2f, 1.0f };
                    draw()->circle( 200.0f, 200.0f, 80.0f, 32, red );
                    const f32 blue[ 4 ] = { 0.2f, 0.4f, 0.9f, 1.0f };
                    draw()->rect( win_w - 160.0f, win_h - 80.0f, 120.0f, 60.0f, blue );
                    draw()->end_pass();
                }

                /* imgui frame: draw the active feature demo plus the picker overlay.  Each
                   demo lives in sb_vulkan_imgui.c and owns its own window(s); the host just
                   dispatches the selected one between new_frame() and render(). */
                if ( imgui() )
                {
                    imgui()->new_frame( win_w, win_h, dt );

                    sb_imgui_demos[ active_demo ].fn();              // selected feature demo
                    active_demo = sb_imgui_demo_picker( active_demo ); // demo list + key hints (clickable)

                    imgui()->render( cmd, win_w, win_h );    // opens LOAD pass on swapchain, flushes, closes pass
                }
                

                /* Only end a frame we actually began.  frame_begin returns an invalid handle
                   without recording (minimized, swapchain out-of-date) -- calling frame_end then
                   would record into a command buffer that was never begun. */
                rhi()->frame_end( ctx );
            }
        }

        /* ------------------------------------------------------------------------------ */

        sys_sleep_milliseconds( 4 );
    }

    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then window.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain; after
       it returns the GPU is idle and pipeline/buffer destroy calls are safe. */

    rhi()->context_destroy( ctx ); // finish rendering and free swapchain + sync objects (first)

    imgui()->shutdown();

    if ( b_use_boot )
    {
        sb_vk_boot_destroy( &boot );    // destroy boot resources
    }

    draw()->shutdown();                 // destroy draw resources

    rhi()->shutdown();                  // destroy device and instance (last)
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
