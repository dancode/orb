/*==============================================================================================

    sandbox/vulkan/sb_vulkan.c -- Vulkan RHI bring-up test.

    Loads sys + app + rhi (static), opens a window, calls rhi()->init() with
    the native HWND, then runs a basic clear-color loop until the window is
    closed or ESC is pressed.  No runtime host, no module hot-reload.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"

// include path: %VULKAN_SDK%\Include    
// library path: %VULKAN_SDK%\Lib

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

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_vulkan] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    /* Open window. */
    win_id_t win = app()->window_open( "sb_vulkan", 0, 0, 1280, 720, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID )
    {
        fprintf( stderr, "[sb_vulkan] window_open failed\n" );
        mod_system_exit();
        return 1;
    }

    /* Hand the native handle to the RHI. */
    void* hwnd = app()->window_handle( win );
    if ( !rhi()->init( hwnd ) )
    {
        fprintf( stderr, "[sb_vulkan] rhi->init failed\n" );
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    printf( "[sb_vulkan] running -- ESC or close window to quit\n" );

    /* Main loop. */
    while ( app()->pump_events() )
    {
        /* Check for resize. */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            if ( ev.type == APP_EV_WIN_RESIZE )
                rhi()->resize( ev.data.win_resize.w, ev.data.win_resize.h );
        }

        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            break;

        /* Frame. */
        rhi_command_list_t cmd = rhi()->frame_begin();
        if ( cmd )
        {
            rhi()->cmd_clear_color( cmd, 0.05f, 0.05f, 0.15f, 1.0f );
            rhi()->frame_end();
        }

        sys_sleep_milliseconds( 16 );
    }

    /* Shutdown. */
    rhi()->shutdown();
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
