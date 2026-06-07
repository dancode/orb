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
#include "sb_vulkan_boot.h"

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
    /* Start render loop. */

    printf( "[sb_vulkan] running -- ESC or close window to quit\n" );

    /* Track window size for viewport/scissor updates. */
    i32 win_w = 1280;
    i32 win_h = 720;

    /* Main loop. */
    while ( app()->pump_events() )
    {
        /* Forward resize events to the context. */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            if ( ev.type == APP_EV_WIN_RESIZE )
            {
                win_w = ev.data.win_resize.w;
                win_h = ev.data.win_resize.h;
                rhi()->context_resize( ctx, win_w, win_h );
            }
        }

        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            break;

        /* ------------------------------------------------------------------------------ */
        /* Render frame -- skip entirely while minimized to avoid 0x0 swapchain churn. */

        if ( !app()->window_is_minimized( win ) )
        {
            rhi_command_list_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                sb_vk_boot_render( &boot, cmd, win_w, win_h );
                rhi()->frame_end( ctx );
            }
        }

        /* ------------------------------------------------------------------------------ */

        sys_sleep_milliseconds( 16 );
    }

    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then window.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain; after
       it returns the GPU is idle and pipeline/buffer destroy calls are safe. */

    rhi()->context_destroy( ctx );
    sb_vk_boot_destroy( &boot );
    draw()->shutdown();
    rhi()->shutdown();
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
