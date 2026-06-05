/*==============================================================================================

    sandbox/vulkan/sb_vulkan.c -- Vulkan RHI bring-up test.

    Loads sys + app + rhi (static), opens a window, calls rhi()->init() then
    rhi()->context_create() with the native HWND, then runs a basic clear-color
    loop until the window is closed or ESC is pressed.  No runtime host, no
    module hot-reload.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"

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

    assert( sys() );
    assert( ref() );
    assert( app() );
    assert( core() );
    assert( rhi() );

    core()->log_set_min_level( LOG_LEVEL_TRACE );

    LOG_LINE();

    /* Open window. */
    win_id_t win = app()->window_open( "sb_vulkan", 0, 0, 1280, 720, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID )
    {
        fprintf( stderr, "[sb_vulkan] window_open failed\n" );
        mod_system_exit();
        return 1;
    }

    /* Global RHI init (instance + device). */
    if ( !rhi()->init() )
    {
        fprintf( stderr, "[sb_vulkan] rhi->init failed\n" );
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    /* Per-window render context. */
    void* hwnd = app()->window_handle( win );
    i32 ctx = rhi()->context_create( win, hwnd, 1280, 720 );
    if ( ctx == RHI_CTX_INVALID )
    {
        fprintf( stderr, "[sb_vulkan] rhi->context_create failed\n" );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    printf( "[sb_vulkan] running -- ESC or close window to quit\n" );

    /* Main loop. */
    while ( app()->pump_events() )
    {
        /* Forward resize events to the context. */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            if ( ev.type == APP_EV_WIN_RESIZE )
                rhi()->context_resize( ctx, ev.data.win_resize.w, ev.data.win_resize.h );
        }

        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            break;

        /* Frame. */
        rhi_command_list_t cmd = rhi()->frame_begin( ctx );
        if ( cmd )
        {
            rhi()->cmd_clear_color( cmd, 0.05f, 0.05f, 0.15f, 1.0f );
            rhi()->frame_end( ctx );
        }

        sys_sleep_milliseconds( 16 );
    }

    /* Shutdown -- context before device, device before window. */
    rhi()->context_destroy( ctx );
    rhi()->shutdown();
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
