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
#include "runtime_service/gui/gui_host.h"
#include "sb_vulkan_boot.h"
#include "sb_vulkan_gui.h"

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
    mod_static( gui );

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
    assert( gui() );

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

         true  -- borderless (window kind 3): no Win32 chrome.  A full-surface GUI_WIN_NATIVE shell
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

    /* Per-window render context -- context_open queries handle + size from app() internally. */
    i32  ctx = rhi()->context_open( win );
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
    /* Setup GUI */

    /* Initialize gui GPU resources and load TrueType font atlas. */
    if ( !gui()->init() )
    {
        fprintf( stderr, "[sb_vulkan] gui->init failed\n" );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }
    
    gui()->font_load( "fonts/jetbrains_regular_16.orb_font" );
 // gui()->font_load( "fonts/jetbrains_regular_24.orb_font" );
 // gui()->font_load( "fonts/jetbrains_bold_24.orb_font" );
    gui()->print_mem_stats();
    
    /* Open the primary viewport for the main window.  This creates its GPU geometry buffers and
       associates win so mouse events from it route to surface 0. */
    gui_vp_t vp0 = gui()->viewport_open( win );
    if ( vp0 == GUI_VP_INVALID ) {
        fprintf( stderr, "[sb_vulkan] gui viewport_open (primary) failed\n" );
        gui()->shutdown();
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
       Toggle b_multi_ctx to exercise the API.  Teardown is handled by gui()->shutdown(). */
    const bool      b_multi_ctx = false;
    gui_ctx_t     ctx2        = GUI_CTX_INVALID;
    if ( b_multi_ctx )
    {
        gui_ctx_config_t game_cfg = GUI_CTX_CONFIG_GAME_UI;
        ctx2 = gui()->ctx_create( &game_cfg );
        /* ctx2 starts deaf; default ctx already listens.  A/S keys switch them below. */
    }
    (void)ctx2;

    /* ------------------------------------------------------------------------------ */
    /* Start render loop. */

    printf( "[sb_vulkan] running -- ESC to quit\n" );
    printf( "[sb_vulkan] gui demos: 1-9 select, +/- step, F1-F4 debug overlay, NP. font scale\n" );
    printf( "[sb_vulkan] P cycles the perf overlay: off -> FPS -> +timings -> +render counts\n" );
    printf( "[sb_vulkan] F6 cycles the render view: normal -> wireframe -> batch colors\n" );

    /* Active gui demo index (see sb_vulkan_gui.c); switched live with the keys below. */
    int active_demo = 0;

    /* Perf overlay detail tier, cycled by P (0 off .. 3 counts).  The overlay itself -- timing,
       smoothing, and draw -- is a built-in gui utility now; the host only supplies a clock and
       the mode (see gui()->perf_overlay below). */
    int perf_mode = 0;

    /* Level 1 idle skip (toggle: I).  When on, the loop blocks on OS input instead of spinning, so a
       static UI burns no frames; wants_redraw() keeps frames flowing while a widget animation plays. */
    bool idle_skip = false;

    /* Main loop. */
    f64 last_time = sys_tick_seconds();
    while ( app()->pump_events() )
    {
        /* Real frame delta (seconds) -- drives gui animation + double-click timing. */
        f64 now_time = sys_tick_seconds();
        f32 dt       = (f32)( now_time - last_time );
        last_time    = now_time;

        /* Drain the app event ring once: gui consumes the input events it cares
           about (text + scroll); the host handles the rest (resize). */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            /* rhi()->event() routes WIN_RESIZE to the matching swapchain (primary or owned floater).
               gui()->event() updates viewport sizes and handles input; it consumes owned-floater
               close events so only the main window's close reaches the host. */
            rhi()->event( &ev );
            if ( gui()->event( &ev ) )
                continue;

            if ( ev.type == APP_EV_WIN_CLOSE )
                goto shutdown;
        }

        /* ------------------------------------------------------------------------------ */
        /* gui demo selection.
           Number keys 1-9 jump straight to a demo; +/- step back and forth through the
           table (with wrap).  The active demo is drawn below, inside the gui frame. */

        bool b_demo_window = true;
        if ( b_demo_window )
        {
            const int demo_count = sb_gui_demo_count();
            // for ( int i = 0; i < demo_count && i < 9; ++i )
            //     if ( app()->key_pressed( (app_key_t)( APP_KEY_1 + i ) ) )
            //         active_demo = i;

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
            gui()->font_set_bmp_scale( bmp_scale );
        }

        /* ------------------------------------------------------------------------------ */
        /* Debug overlay layers (Debug build only): toggle each with the F1-F4 keys.
           F1 window frames   F2 widget interaction rects   F3 resize bands   F4 clip rects. */

        gui()->debug_enable( true );

        /* ------------------------------------------------------------------------------ */
        /* Perf overlay: P cycles off -> FPS -> +timings -> +render counts (mod 4).  The library
           measures and smooths; the host only carries the tier. */

        if ( app()->key_pressed( APP_KEY_P ) )
            perf_mode = ( perf_mode + 1 ) % 5;

        /* F6 cycles the debug render view: normal -> wireframe (triangle edges) -> batch (per-draw
           color tint, so you can count batches and see where they split) -> normal. */
        if ( app()->key_pressed( APP_KEY_F9 ) )
        {
            gui_render_mode_t m = ( gui()->debug_get_render_mode() + 1 ) % GUI_RENDER_MODE_COUNT;
            gui()->debug_set_render_mode( m );
            static const char* names[] = { "normal", "wireframe", "batch" };
            printf( "[sb_vulkan] render mode: %s\n", names[ m ] );
        }

        /* I toggles Level 1 idle skip (block-on-input vs spin); see the frame-pacing section below. */
        if ( app()->key_pressed( APP_KEY_I ) )
        {
            idle_skip = !idle_skip;
            printf( "[sb_vulkan] idle skip: %s\n", idle_skip ? "on (block on input)" : "off (spin)" );
        }

        /* C toggles Level 2 retained skip: skips tessellation on unchanged frames (hash upfront). */
        if ( app()->key_pressed( APP_KEY_C ) )
        {
            bool on = !gui()->retained_skip();
            gui()->set_retained_skip( on );
            printf( "[sb_vulkan] retained skip: %s\n", on ? "on (skip tess if unchanged)" : "off (always tess)" );
        }
       
        /* ------------------------------------------------------------------------------ */
        /* Build the UI.  Every begin has a matching end -- the frame is a balanced scope:

            frame_begin(dt)              -- global once-per-frame; poll input, compute dirty.
            if ( frame_dirty() )         -- skip emit entirely on provably clean frames.
              ctx_begin(GUI_CTX_DEFAULT) -- bind + init the default context; emit its windows.
              ctx_end()                    -- close it.
              ctx_begin(ctx2)              -- the secondary context (multi-ctx demo); emit its windows.
              ctx_end()
            frame_end()                  -- seal the build (no-op on clean frames).
           Emit windows IMMEDIATELY after each ctx_begin -- it leaves that context bound, so windows
           land in the correct pool. */

        const bool b_multi = ( b_multi_ctx && ctx2 != GUI_CTX_INVALID );

        /* OR'd across both contexts: does any widget still need another frame (mid-animation)?  Each
           context clears its own flag at ctx_begin and sets it during emit, so it is captured below
           while that context is still bound, before ctx_end rebinds away. */

        bool any_redraw = false;
        gui()->frame_begin( dt );

        /* 'A'/'S' switch which context receives input (multi-ctx demo; reads s_io after frame_begin).
           Runs unconditionally -- key-press events are consumed regardless of dirty state. */
        if ( b_multi )
        {
            if ( gui()->is_key_pressed( APP_KEY_A ) )
            {
                gui()->ctx_set_listening( GUI_CTX_DEFAULT, true  );
                gui()->ctx_set_listening( ctx2,              false );
            }
            if ( gui()->is_key_pressed( APP_KEY_S ) )
            {
                gui()->ctx_set_listening( GUI_CTX_DEFAULT, false );
                gui()->ctx_set_listening( ctx2,              true  );
            }
        }

        /* Skip widget emit entirely when input, animation, and render state are all unchanged.
           render() below will reuse the preserved tessellation from the previous clean frame. */
        if ( gui()->frame_dirty() )
        {
            bool b_dbg_dirty = false;
            if ( b_dbg_dirty ) {
                printf( "frame dirty: dt %.3f, wants_redraw %d \n", dt, gui()->wants_redraw() );
            }

            /* --- Default context: the main build + perf overlay live in one ctx scope. --- */
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            /* Borderless main window: this full-surface native shell IS the OS window's frame. */
            if ( b_borderless )
            {
                gui()->window_begin( "ORB -- sb_vulkan", GUI_WIN_NATIVE | GUI_WIN_NOSCROLL );
                gui()->window_end();
            }

            /* The active demo runs in the single-context path; the multi-ctx demo shows ctx2 instead. */
            if ( b_demo_window && !b_multi )
            {
                sb_gui_demos[ active_demo ].fn();
                active_demo = sb_gui_demo_picker( active_demo );
            }

            bool second_surface = false;
            if ( second_surface )
            {
                gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
                gui()->window_set_next_size( 360, 240, GUI_COND_ONCE );
                if ( gui()->window_begin( "Second Surface", GUI_WIN_NONE ) )
                {
                    gui()->stack();
                    gui()->text( "Detach me: click the title-bar button" );
                    gui()->text( "or drag my title past the window edge." );
                    gui()->separator();
                    gui()->text( "Once detached I am a native borderless" );
                    gui()->text( "window -- drag my title bar to move the" );
                    gui()->text( "OS window, drag my borders to resize." );
                }
                gui()->window_end();
            }

            /* Perf overlay -- last so it draws on top, inside the default context's scope and the build
               so its own text is counted in the emit + render cost it reports.  Emit opens at frame_begin
               and closes at frame_end; render is summed across render() below. */
            gui()->perf_overlay( sys_tick_seconds, perf_mode );

            any_redraw |= gui()->wants_redraw();   /* default context's animation state, still bound */
            gui()->ctx_end();

            /* --- Secondary context (multi-ctx demo): its own scope. --- */
            if ( b_multi )
            {
                gui()->ctx_begin( ctx2 );
                {
                    static int ctx2_click_count = 0;
                    gui()->window_set_next_pos ( 700, 60, GUI_COND_ONCE );
                    gui()->window_set_next_size( 280, 200, GUI_COND_ONCE );
                    if ( gui()->window_begin( "Context 2 Window", GUI_WIN_NONE ) )
                    {
                        gui()->stack();
                        gui()->text( "Secondary context (ctx2)." );
                        gui()->text( "Press A: ctx1 listens (default)." );
                        gui()->text( "Press S: ctx2 listens." );
                        gui()->separator();
                        if ( gui()->button( "Click me (ctx2)" ) ) ++ctx2_click_count;
                        gui()->textf( "Clicks: %d", ctx2_click_count );
                    }
                    gui()->window_end();
                }
                any_redraw |= gui()->wants_redraw();   /* ctx2 animation state, still bound */
                gui()->ctx_end();
            }

        } /* if ( frame_dirty() ) */

        gui()->frame_end();

        /* ------------------------------------------------------------------------------ */
        /* Reconcile gui-owned floaters with their OS windows (destroys any the user closed).
           Runs after the build and before any present -- the safe point to tear a surface down. */

        gui()->viewport_update();

        /* ------------------------------------------------------------------------------ */
        /* Flush the main surface.  Skip while minimized to avoid 0x0 swapchain churn; frame_begin
           returns an invalid handle then and we must not frame_end it.  gui brackets its own
           render cost inside render() / viewport_render_floaters() for the perf overlay. */

        if ( !app()->window_is_minimized( win ) )
        {
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                if ( b_use_boot )
                {
                    i32 bw = 0, bh = 0;
                    app()->window_get_size( win, &bw, &bh );
                    sb_vk_boot_render( &boot, cmd, bw, bh );
                }
                else
                {
                    /* Clear so gui's LOAD pass composites over a fresh background instead of last
                       frame's pixels (otherwise dragging a window smears: hall-of-mirrors). */
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );
                }

                gui()->render( vp0, cmd );    // primary partition + debug overlay
                rhi()->frame_end( ctx );
            }
        }

        /* ------------------------------------------------------------------------------ */
        /* Present every gui-owned floater from the SAME draw list.  gui opens each floater's
           own rhi context frame, clears, replays that viewport's partition, and ends -- the host
           no longer hand-drives the secondary surface. */

        gui()->viewport_render_floaters();

        /* ------------------------------------------------------------------------------ */
        /* Frame pacing.  Default: spin at ~250 Hz (game cadence).  Idle-skip on: mirror the editor
           host -- block on OS input so a static UI costs no frames, but while any widget animates
           (any_redraw) keep running at ~60 Hz so the transition finishes before sleeping on input. */
        if ( idle_skip )
        {
            if ( any_redraw )
                sys_sleep_milliseconds( 16 );        /* animating: ~60fps until it settles */
            else
                sys_wait_for_os_events_ms( 500 );    /* idle: wake on input (500 ms safety cap) */
        }
        else
            sys_sleep_milliseconds( 4 );
    }

shutdown:
    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then windows.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain.  The main
       context is the host's; the gui-owned floater's context + window + geometry are torn down
       inside gui()->shutdown() (it owns them), so the host does not touch ctx2/win2/vp1. */

    rhi()->context_destroy( ctx );        // idle + free the main swapchain/sync (host-owned)

    gui()->shutdown();                  // frees all geometry + the owned floater's ctx/window

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
