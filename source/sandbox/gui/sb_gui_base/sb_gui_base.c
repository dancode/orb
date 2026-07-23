/*==============================================================================================

    sandbox/gui/sb_gui_base/sb_gui_base.c -- the minimal gui frame loop.

    The smallest program that gets a 2D frame through the pipeline and onto the screen: boot
    (window + rhi + gui context), then per-frame frame_poll -> frame_begin -> ctx_begin ->
    (nothing) -> ctx_end -> frame_end -> present_begin -> present_end -> frame_pace.  No
    widgets, no demo content -- the body between ctx_begin/ctx_end is deliberately empty.

    Reference for what gui()->boot sets up and what every other gui sandbox builds on top of.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_base] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* One-call setup: gui owns the main window + render context end to end. */
    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui base",
        .w         = 1280, .h = 720,
        .os_chrome = true,   /* stock OS-framed window instead of the gui-driven borderless viewport */
        .font      = GUI_FONT_CASCADIA_MONO_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.15f, 0.15f, 0.20f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_base] gui->boot failed\n" );
        goto shutdown;
    }

    f32 dt = 0.0f;
    while ( gui()->frame_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            // gui()->ctx_begin( GUI_CTX_DEFAULT );

            /* The bare minimum to get a rect on screen: draw_rect pushes straight into the draw
               list under the ambient clip/z -- no window, pane, or region needed to place it. */
            gui()->draw_rect( 100.0f, 100.0f, 200.0f, 120.0f, GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF ) );

            // gui()->ctx_end();
        }
        gui()->frame_end();

        /* present_begin opens the main surface's frame (cleared to the boot color); present_end
           draws the gui and presents.  This pair is what places the 2D frame in the backend. */
        gui()->present_begin( NULL );
        gui()->present_end();

        gui()->frame_pace( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();   /* also tears down the boot window + context */
    rhi()->shutdown();                                 /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

// clang-format on
