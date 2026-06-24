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

    core()->log_set_min_level( LOG_LEVEL_INFO ); // LOG_LEVEL_TRACE
    core_log_fn( LOG_LEVEL_DEBUG, "sb_vulkan", "debug log: modules loaded successfully" );

    LOG_LINE();

    /* ------------------------------------------------------------------------------ */
    /* Setup RHI + Window */

    /* Global RHI init (instance + device). */
    if ( !rhi()->init() ) {
         mod_system_exit();
         return 1;
    }

    /* Main-window chrome mode -- flip this one bool to compare the two paths:

         true  -- borderless (window kind 3): no Win32 chrome.  A full-surface IMGUI_WIN_NATIVE shell
                  window (emitted first in the loop below) stands in as the frame: its titlebar drives
                  OS move + the min/max/close caption buttons, its borders resize via WM_NCHITTEST.

         false -- default OS window: Win32 draws the title bar, caption buttons, and borders.  The
                  native frame-shell is NOT emitted; the demos draw straight over the cleared surface. */

    const bool b_borderless = false;

    win_id_t win = app()->window_open( "sb_vulkan", 0, 0, 1280, 720, b_borderless ? APP_WIN_BORDERLESS : APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID ) {
         rhi()->shutdown();
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

    /* ------------------------------------------------------------------------------ */
    /* Setup IMGUI */

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
    
    imgui()->load_font( "fonts/jetbrains_regular_16.orb_font" );
 // imgui()->load_font( "fonts/jetbrains_regular_24.orb_font" );
 // imgui()->load_font( "fonts/jetbrains_bold_24.orb_font" );
    imgui()->print_mem_stats();
    
    /* Initial drawable sizes (updated by resize events in the loop below). */
    i32 win_w = 1280;
    i32 win_h = 720;

    /* Open the primary viewport for the main window.  This creates its GPU geometry buffers and
       associates win so mouse events from it route to surface 0. */
    imgui_vp_t vp0 = imgui()->viewport_open( win, win_w, win_h );
    if ( vp0 == IMGUI_VP_INVALID ) {
        fprintf( stderr, "[sb_vulkan] imgui viewport_open (primary) failed\n" );
        imgui()->shutdown();
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    /* Single window at startup -- everything is built into the main viewport (0).  The detach
       gesture (title-bar button, or dragging a panel past the window edge) spawns owned floaters
       on demand, each a native-borderless OS window. */

    /* Multi-context demo: create a secondary context with game-UI sizing (small pools).
       Default context starts listening; ctx2 starts deaf.  A/S keys toggle which listens.
       Toggle b_multi_ctx to exercise the API.  Teardown is handled by imgui()->shutdown(). */
    const bool      b_multi_ctx = false;
    imgui_ctx_t     ctx2        = IMGUI_CTX_INVALID;
    if ( b_multi_ctx )
    {
        imgui_ctx_config_t game_cfg = IMGUI_CTX_CONFIG_GAME_UI;
        ctx2 = imgui()->ctx_create( &game_cfg );
        /* ctx2 starts deaf; default ctx already listens.  A/S keys switch them below. */
    }
    (void)ctx2;

    /* ------------------------------------------------------------------------------ */
    /* Start render loop. */

    printf( "[sb_vulkan] running -- ESC to quit\n" );
    printf( "[sb_vulkan] imgui demos: 1-9 select, +/- step, F1-F4 debug overlay, NP. font scale\n" );
    printf( "[sb_vulkan] P cycles the perf overlay: off -> FPS -> +timings -> +render counts\n" );
    printf( "[sb_vulkan] F6 cycles the render view: normal -> wireframe -> batch colors\n" );

    /* Active imgui demo index (see sb_vulkan_imgui.c); switched live with the keys below. */
    int active_demo = 0;

    /* Perf overlay detail tier, cycled by P (0 off .. 3 counts).  The overlay itself -- timing,
       smoothing, and draw -- is a built-in imgui utility now; the host only supplies a clock and
       the mode (see imgui()->perf_overlay below). */
    int perf_mode = 0;

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
                    /* The main window's resize is the host's to route; the floater's is consumed by
                       imgui()->event() above (imgui owns that window + context). */
                    if ( ev.win_id == win )
                    {
                        win_w = ev.data.win_resize.w;
                        win_h = ev.data.win_resize.h;
                        rhi()->context_resize( ctx, win_w, win_h );
                        imgui()->viewport_resize( vp0, win_w, win_h );
                    }
                    break;

                case APP_EV_WIN_CLOSE:
                    /* The floater's close is consumed by imgui()->event() (it tears down its own
                       surface); only the main window's close reaches here and ends the test. */
                    goto shutdown;

                default:
                    break;
            }
        }

        /* ------------------------------------------------------------------------------ */
        /* imgui demo selection.
           Number keys 1-9 jump straight to a demo; +/- step back and forth through the
           table (with wrap).  The active demo is drawn below, inside the imgui frame. */

        bool b_demo_window = true;
        if ( b_demo_window )
        {
            const int demo_count = sb_imgui_demo_count();
            for ( int i = 0; i < demo_count && i < 9; ++i )
                if ( app()->key_pressed( (app_key_t)( APP_KEY_1 + i ) ) )
                    active_demo = i;

            if ( app()->key_pressed( APP_KEY_NP_ADD ) )
                active_demo = ( active_demo + 1 ) % demo_count;
            if ( app()->key_pressed( APP_KEY_NP_SUB ) )
                active_demo = ( active_demo + demo_count - 1 ) % demo_count;
        }

        /* ------------------------------------------------------------------------------ */
        /* Bitmap font scale cycle (1x/2x/3x) stays handy on the numpad dot. */

        if ( app()->key_pressed( APP_KEY_NP_DOT ) )
        {
            static int bmp_scale = 1;
            bmp_scale = bmp_scale == 1 ? 2 : ( bmp_scale == 2 ? 3 : 1 );
            imgui()->set_bmp_scale( bmp_scale );
        }

        /* ------------------------------------------------------------------------------ */
        /* Debug overlay layers (Debug build only): toggle each with the F1-F4 keys.
           F1 window frames   F2 widget interaction rects   F3 resize bands   F4 clip rects. */

        bool b_dbg_overlay = true;
        if ( b_dbg_overlay )
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
        /* Perf overlay: P cycles off -> FPS -> +timings -> +render counts (mod 4).  The library
           measures and smooths; the host only carries the tier. */

        if ( app()->key_pressed( APP_KEY_P ) )
            perf_mode = ( perf_mode + 1 ) % 4;

        /* F6 cycles the debug render view: normal -> wireframe (triangle edges) -> batch (per-draw
           color tint, so you can count batches and see where they split) -> normal. */
        if ( app()->key_pressed( APP_KEY_F6 ) )
        {
            imgui_render_mode_t m = ( imgui()->debug_get_render_mode() + 1 ) % IMGUI_RENDER_MODE_COUNT;
            imgui()->debug_set_render_mode( m );
            static const char* names[] = { "normal", "wireframe", "batch" };
            printf( "[sb_vulkan] render mode: %s\n", names[ m ] );
        }

        /* ------------------------------------------------------------------------------ */
        /* Build the UI.  Every begin has a matching end -- the frame is a balanced scope:

            frame_begin(dt)              -- global once-per-frame; binds no context.
              ctx_begin(IMGUI_CTX_DEFAULT) -- bind + init the default context; emit its windows.
              ctx_end()                    -- close it.
              ctx_begin(ctx2)              -- the secondary context (multi-ctx demo); emit its windows.
              ctx_end()
            frame_end()                  -- seal the build.
           Emit windows IMMEDIATELY after each ctx_begin -- it leaves that context bound, so windows
           land in the correct pool. */

        const bool b_multi = ( b_multi_ctx && ctx2 != IMGUI_CTX_INVALID );

        imgui()->frame_begin( dt );

        /* 'A'/'S' switch which context receives input (multi-ctx demo; reads s_io after frame_begin). */
        if ( b_multi )
        {
            if ( imgui()->is_key_pressed( APP_KEY_A ) )
            {
                imgui()->ctx_set_listening( IMGUI_CTX_DEFAULT, true  );
                imgui()->ctx_set_listening( ctx2,              false );
            }
            if ( imgui()->is_key_pressed( APP_KEY_S ) )
            {
                imgui()->ctx_set_listening( IMGUI_CTX_DEFAULT, false );
                imgui()->ctx_set_listening( ctx2,              true  );
            }
        }

        /* --- Default context: the main build + perf overlay live in one ctx scope. --- */
        imgui()->ctx_begin( IMGUI_CTX_DEFAULT );

        /* Borderless main window: this full-surface native shell IS the OS window's frame. */
        if ( b_borderless )
        {
            imgui()->begin_window( "ORB -- sb_vulkan", IMGUI_WIN_NATIVE | IMGUI_WIN_NOSCROLL );
            imgui()->end_window();
        }

        /* The active demo runs in the single-context path; the multi-ctx demo shows ctx2 instead. */
        if ( b_demo_window && !b_multi )
        {
            sb_imgui_demos[ active_demo ].fn();
            active_demo = sb_imgui_demo_picker( active_demo );
        }

        bool second_surface = false;
        if ( second_surface )
        {
            imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
            imgui()->set_next_window_size( 360, 240, IMGUI_COND_ONCE );
            if ( imgui()->begin_window( "Second Surface", IMGUI_WIN_NONE ) )
            {
                imgui()->stack();
                imgui()->text( "Detach me: click the title-bar button" );
                imgui()->text( "or drag my title past the window edge." );
                imgui()->separator();
                imgui()->text( "Once detached I am a native borderless" );
                imgui()->text( "window -- drag my title bar to move the" );
                imgui()->text( "OS window, drag my borders to resize." );
            }
            imgui()->end_window();
        }

        /* Perf overlay -- last so it draws on top, inside the default context's scope and the build
           so its own text is counted in the emit + render cost it reports.  Emit opens at frame_begin
           and closes at frame_end; render is summed across render() below. */
        imgui()->perf_overlay( sys_tick_seconds, perf_mode );

        imgui()->ctx_end();

        /* --- Secondary context (multi-ctx demo): its own scope. --- */
        if ( b_multi )
        {
            imgui()->ctx_begin( ctx2 );
            {
                static int ctx2_click_count = 0;
                imgui()->set_next_window_pos ( 700, 60, IMGUI_COND_ONCE );
                imgui()->set_next_window_size( 280, 200, IMGUI_COND_ONCE );
                if ( imgui()->begin_window( "Context 2 Window", IMGUI_WIN_NONE ) )
                {
                    imgui()->stack();
                    imgui()->text( "Secondary context (ctx2)." );
                    imgui()->text( "Press A: ctx1 listens (default)." );
                    imgui()->text( "Press S: ctx2 listens." );
                    imgui()->separator();
                    if ( imgui()->button( "Click me (ctx2)" ) ) ++ctx2_click_count;
                    imgui()->textf( "Clicks: %d", ctx2_click_count );
                }
                imgui()->end_window();
            }
            imgui()->ctx_end();
        }

        imgui()->frame_end();

        /* ------------------------------------------------------------------------------ */
        /* Reconcile imgui-owned floaters with their OS windows (destroys any the user closed).
           Runs after the build and before any present -- the safe point to tear a surface down. */

        imgui()->update_platform_windows();

        /* ------------------------------------------------------------------------------ */
        /* Flush the main surface.  Skip while minimized to avoid 0x0 swapchain churn; frame_begin
           returns an invalid handle then and we must not frame_end it.  imgui brackets its own
           render cost inside render() / render_floaters() for the perf overlay. */

        if ( !app()->window_is_minimized( win ) )
        {
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                if ( b_use_boot )
                {
                    sb_vk_boot_render( &boot, cmd, win_w, win_h );
                }
                else
                {
                    /* Clear so imgui's LOAD pass composites over a fresh background instead of last
                       frame's pixels (otherwise dragging a window smears: hall-of-mirrors). */
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );
                }

                imgui()->render( vp0, cmd );    // primary partition + debug overlay
                rhi()->frame_end( ctx );
            }
        }

        /* ------------------------------------------------------------------------------ */
        /* Present every imgui-owned floater from the SAME draw list.  imgui opens each floater's
           own rhi context frame, clears, replays that viewport's partition, and ends -- the host
           no longer hand-drives the secondary surface. */

        imgui()->render_floaters();

        /* ------------------------------------------------------------------------------ */

        sys_sleep_milliseconds( 4 );
    }

shutdown:
    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then windows.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain.  The main
       context is the host's; the imgui-owned floater's context + window + geometry are torn down
       inside imgui()->shutdown() (it owns them), so the host does not touch ctx2/win2/vp1. */

    rhi()->context_destroy( ctx );        // idle + free the main swapchain/sync (host-owned)

    imgui()->shutdown();                  // frees all geometry + the owned floater's ctx/window

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
