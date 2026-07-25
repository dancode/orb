/*==============================================================================================

    sandbox/rhi/sb_vulkan/sb_vulkan.c -- Vulkan RHI bring-up test.

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline: the
    sb_vk_boot triangle renders every frame until the window is closed.  A shader_tool override
    pair (.spv or cooked .oshd) next to the exe swaps the embedded shaders for baked ones -- see
    sb_vulkan_boot.h for the recipes.  No runtime host, no module hot-reload.

    gui is loaded as an INSTRUMENT, not a subject: one small status window reports what the
    renderer is doing (shader source, surface size, frame time) and toggles the triangle pass
    so gui-over-render composition is verifiable.  The gui feature tour lives in sb_gui_example.

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

    /* A render test wants the plain OS window: Win32 chrome, no gui shell standing in. */
    win_id_t win = app()->window_open( "sb_vulkan", 0, 0, 1280, 720, APP_WIN_DEFAULT );
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

    /* Boot triangle -- embedded SPIR-V by default; a .spv/.oshd pair next to the exe overrides
       it (boot.dxc / boot.oshd report which source won; see sb_vulkan_boot.h). */
    sb_vk_boot_t boot = { 0 };
    if ( !sb_vk_boot_create( &boot ) )
    {
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    /* ------------------------------------------------------------------------------ */
    /* Setup GUI -- the render-status overlay (see the file header). */

    if ( !gui()->init( GUI_FONT_JETBRAINS_16 ) )
    {
        fprintf( stderr, "[sb_vulkan] gui->init failed\n" );
        sb_vk_boot_destroy( &boot );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    gui_vp_t vp0 = gui()->viewport_open( win );
    if ( vp0 == GUI_VP_INVALID ) {
        fprintf( stderr, "[sb_vulkan] gui viewport_open (primary) failed\n" );
        gui()->shutdown();
        sb_vk_boot_destroy( &boot );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    /* The frame_pace clock + sleep hooks (gui links only app + rhi, so the host wires sys). */
    gui()->frame_set_hooks( sys_tick_seconds, sys_sleep_milliseconds, sys_wait_for_os_events_ms );

    /* Overlay state: toggling the triangle swaps between the boot pass (its own clear) and a
       bare host clear -- both paths must composite the gui correctly. */
    bool b_draw_triangle = true;

    /* ------------------------------------------------------------------------------ */
    /* Main loop. */

    f64 last_time = sys_tick_seconds();
    while ( app()->pump_events() )
    {
        f64 now_time = sys_tick_seconds();
        f32 dt       = (f32)( now_time - last_time );
        last_time    = now_time;

        /* Drain the app event ring once: rhi routes WIN_RESIZE to the swapchain, gui consumes
           the input events it cares about, the host keeps only the close.  Routing stops at the
           first sink answering APP_EVENT_CONSUMED (app_event_result_t, app.h). */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            if ( rhi()->event( &ev ) == APP_EVENT_CONSUMED )
                continue;
            if ( gui()->event( &ev ) == APP_EVENT_CONSUMED )
                continue;

            if ( ev.type == APP_EV_WIN_CLOSE )
                goto shutdown;
        }

        /* ------------------------------------------------------------------------------ */
        /* Build the status overlay -- one window, balanced frame scope. */

        i32 win_w = 0, win_h = 0;
        app()->window_get_size( win, &win_w, &win_h );

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            gui()->window_set_next_pos ( 20, 20, GUI_COND_ONCE );
            gui()->window_set_next_size( 300, 190, GUI_COND_ONCE );
            if ( gui()->window_begin( "Render Status", GUI_WIN_NONE ) )
            {
                gui()->stack();
                gui()->textf( "shaders: %s", boot.oshd ? "cooked .oshd override"
                                           : boot.dxc  ? "dxc .spv override"
                                                       : "embedded SPIR-V" );
                gui()->textf( "surface: %d x %d", win_w, win_h );
                gui()->textf( "frame:   %.2f ms (%.0f fps)", dt * 1000.0f, dt > 0.0f ? 1.0f / dt : 0.0f );
                gui()->separator();
                gui()->checkbox( "Draw boot triangle", &b_draw_triangle );
                gui()->text( b_draw_triangle ? "boot pass: clear + triangle"
                                             : "host pass: clear only" );
            }
            gui()->window_end();

            gui()->ctx_end();
        }
        gui()->frame_end();

        /* ------------------------------------------------------------------------------ */
        /* Render.  The boot pass clears + draws the triangle; with it off the host clears so
           gui's LOAD pass still composites over a fresh background.  Skip while minimized to
           avoid 0x0 swapchain churn. */

        if ( !app()->window_is_minimized( win ) )
        {
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                if ( b_draw_triangle )
                {
                    sb_vk_boot_render( &boot, cmd, win_w, win_h );
                }
                else
                {
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );
                }

                gui()->render( vp0, cmd );    // status overlay composites over the render
                rhi()->frame_end( ctx );
            }
        }

        /* Frame pacing: spin at 4 ms (~250 Hz) -- a render test wants continuous redraws. */
        gui()->frame_pace( 4, 16 );
    }

shutdown:
    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then windows.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain. */

    rhi()->context_destroy( ctx );      // idle + free the main swapchain/sync (host-owned)

    gui()->shutdown();                  // frees gui geometry

    sb_vk_boot_destroy( &boot );        // destroy boot resources

    draw()->shutdown();                 // destroy draw resources

    rhi()->shutdown();                  // destroy device and instance (last)
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
