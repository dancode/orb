/*==============================================================================================

    sandbox/gui/sb_gui_example/sb_gui_example.c -- gui feature explorer.

    The end-to-end tour of the gui system: every feature group has a demo window (ex_demos.c)
    and the "Demos" picker steps through them.  This host owns the standard gui frame -- module
    boot, one borderless main window whose viewport_shell() is the chrome, the balanced
    frame_begin/ctx_begin .. ctx_end/frame_end build, render + floater present, frame pacing.
    Rendering itself is not under test here (see sb_vulkan for that); the host just clears the
    surface and composites the gui over it.

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
#include "ex_demos.h"

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
        fprintf( stderr, "[sb_gui_example] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    core()->log_set_min_level( LOG_LEVEL_INFO );

    LOG_LINE();

    /* ------------------------------------------------------------------------------ */
    /* Setup RHI + Window */

    /* Global RHI init (instance + device). */
    if ( !rhi()->init() ) {
         mod_system_exit();
         return 1;
    }

    /* Main-window chrome mode -- flip this one bool to compare the two paths:

         true  -- borderless (window kind 3): no Win32 chrome.  gui()->viewport_shell() (emitted
                  first in the loop below) stands in as the frame: its titlebar drives OS move +
                  the min/max/close caption buttons, its borders resize via WM_NCHITTEST.

         false -- default OS window: Win32 draws the title bar, caption buttons, and borders.
                  viewport_shell() no-ops; the demos draw straight over the cleared surface. */

    const bool b_borderless = true;

    win_id_t win = app()->window_open( "sb_gui_example", 0, 0, 1280, 720, b_borderless ? APP_WIN_BORDERLESS : APP_WIN_DEFAULT );
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

    /* Initialize draw GPU resources (buffers + pipelines) now that the device is live. */
    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_gui_example] draw->init failed\n" );
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    /* ------------------------------------------------------------------------------ */
    /* Setup GUI */

    /* Initialize gui GPU resources and load the built-in font atlas. */
    if ( !gui()->init( GUI_FONT_JETBRAINS_16 ) )
    {
        fprintf( stderr, "[sb_gui_example] gui->init failed\n" );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    gui()->print_mem_stats();

    /* Open the primary viewport for the main window.  This creates its GPU geometry buffers and
       associates win so mouse events from it route to surface 0. */
    gui_vp_t vp0 = gui()->viewport_open( win );
    if ( vp0 == GUI_VP_INVALID ) {
        fprintf( stderr, "[sb_gui_example] gui viewport_open (primary) failed\n" );
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
    const bool b_multi_ctx = false;
    gui_ctx_id_t  ctx2 = GUI_CTX_INVALID;
    if ( b_multi_ctx )
    {
        gui_ctx_config_t game_cfg = GUI_CTX_CONFIG_GAME_UI;
        ctx2 = gui()->ctx_create( &game_cfg );
        /* ctx2 starts deaf; default ctx already listens.  A/S keys switch them below. */
    }
    (void)ctx2;

    /* ------------------------------------------------------------------------------ */
    /* Start render loop. */

    /* OS services gui cannot reach itself (it links only app + rhi): the perf-overlay clock and
       the frame_pace sleep / idle wait.  Wired once; the loop below just calls frame_pace(). */
    gui()->set_frame_hooks( sys_tick_seconds, sys_sleep_milliseconds, sys_wait_for_os_events_ms );

    /* Debug driver: gui owns the debug hotkeys + overlay emission from here on (the hotkeys
       printed above -- see debug_enable in gui_api.h). */
    gui()->debug_enable( true );

    /* Active gui demo index (see ex_demos.c); switched live with the keys below. */
    int active_demo = 0;

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
        /* gui demo selection: +/- step back and forth through the table (with wrap).
           The active demo is drawn below, inside the gui frame. */

        {
            const int demo_count = ex_demo_count();
            if ( app()->key_pressed( APP_KEY_NP_ADD ) )
                active_demo = ( active_demo + 1 ) % demo_count;
            if ( app()->key_pressed( APP_KEY_NP_SUB ) )
                active_demo = ( active_demo + demo_count - 1 ) % demo_count;
        }

        /* ------------------------------------------------------------------------------ */
        /* Build the UI.  Every begin has a matching end -- the frame is a balanced scope:

            if ( frame_begin(dt) )       -- global once-per-frame; poll input, returns frame_dirty
                                            (emit is skipped entirely on provably clean frames).
              ctx_begin(GUI_CTX_DEFAULT) -- bind + init the default context; emit its windows.
              ctx_end()                    -- close it (debug overlays auto-emit here).
              ctx_begin(ctx2)              -- the secondary context (multi-ctx demo); emit its windows.
              ctx_end()
            frame_end()                  -- seal the build (volatile replay on clean frames).
           Emit windows IMMEDIATELY after each ctx_begin -- it leaves that context bound, so windows
           land in the correct pool.  Debug hotkeys (P/O/F9/F10/C/I) are handled inside gui. */

        const bool b_multi = ( b_multi_ctx && ctx2 != GUI_CTX_INVALID );

        bool frame_dirty = gui()->frame_begin( dt );

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
        if ( frame_dirty )
        {
            /* --- Default context: the main build lives in one ctx scope. --- */
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            /* Borderless main window: the shell IS the OS window's frame (no-op with OS chrome). */
            gui()->viewport_shell( vp0, "ORB -- gui example", GUI_WIN_NONE );

            /* The active demo runs in the single-context path; the multi-ctx demo shows ctx2 instead. */
            if ( !b_multi )
            {
                ex_demos[ active_demo ].fn();
                active_demo = ex_demo_picker( active_demo );
            }

            /* Closing the default context also auto-emits the debug overlays (perf/state/dashboard)
               last in its build, so they draw on top and count in the cost they report. */
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
                gui()->ctx_end();
            }

        } /* if ( frame_dirty ) */

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
                /* Clear so gui's LOAD pass composites over a fresh background instead of last
                   frame's pixels (otherwise dragging a window smears: hall-of-mirrors). */
                rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                    .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                    .load_op  = RHI_LOAD_OP_CLEAR,
                    .store_op = RHI_STORE_OP_STORE,
                    .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                }, 1, NULL );
                rhi()->cmd_end_rendering( cmd );

                gui()->render( vp0, cmd );    // primary partition + debug overlay
                rhi()->frame_end( ctx );
            }
        }

        /* ------------------------------------------------------------------------------ */
        /* Present every gui-owned floater from the SAME draw list.  gui opens each floater's
           own rhi context frame, clears, replays that viewport's partition, and ends -- the host
           no longer hand-drives the secondary surface. */

        gui()->viewport_render_floaters();

        /* Frame pacing (built-in): spin at 4 ms (~250 Hz) by default; with idle skip on (I) block
           on OS input while the UI is static, 16 ms (~60 Hz) while a widget animation settles. */
        gui()->frame_pace( 4, 16 );
    }

shutdown:
    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then windows.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain.  The main
       context is the host's; gui-owned floater contexts + windows + geometry are torn down
       inside gui()->shutdown() (it owns them). */

    rhi()->context_destroy( ctx );      // idle + free the main swapchain/sync (host-owned)

    gui()->shutdown();                  // frees all geometry + any owned floater's ctx/window

    draw()->shutdown();                 // destroy draw resources

    rhi()->shutdown();                  // destroy device and instance (last)
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
