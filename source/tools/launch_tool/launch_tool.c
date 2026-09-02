/*==============================================================================================

    tools/launch_tool/launch_tool.c -- ORB launcher: project manager / command hub.

    Unity build entry.  The launcher is the engine's one gui()-driven tool: a minimal gui
    app (engine floor + rhi + gui, no runtime) that wraps the command-line workflow --
    create/open projects, run doctor, launch hosts -- by spawning the same commands a user
    would type.  Boot follows the gui boot path (gui owns the window + render context).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/res/res.h"
#include "engine/pack/pack_host.h"
#include "engine/fs/fs_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

#include "tools/launch_tool/launch_tool.h"

/*==============================================================================================
    Shared state -- defined before the units so every unit sees it
==============================================================================================*/

static launch_state_t s_launch;

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "tools/launch_tool/launch_registry.c"
#include "tools/launch_tool/launch_ui.c"

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
    mod_static( pack );
    mod_static( fs );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[launch_tool] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* One-call setup: gui owns the main window + render context end to end (boot path). */
    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB Launcher",
        .w     = 1280, .h = 720,
        .font  = RID( "font/jetbrains/16" ),
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[launch_tool] gui->boot failed\n" );
        goto shutdown;
    }

    launch_ui_init();

    gui()->debug_enable( true ); /* hotkeys: P=perf, O=state, F10=pipeline dashboard */

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin();
            launch_ui_frame( vp0 );
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        gui()->boot_pace ( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();  /* also tears down the boot window + context */
    rhi()->shutdown();                               /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
