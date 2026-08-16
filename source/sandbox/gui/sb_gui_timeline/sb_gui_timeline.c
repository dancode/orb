/*==============================================================================================

    sandbox/gui/sb_gui_timeline/sb_gui_timeline.c - prof timeline overlay test bed.

    Two windows over the gui boot shell:

      Profiler Timeline  -- the flame-graph overlay (gui_timeline.c, transplantable)
      Workload           -- toggleable busy-work scenarios that feed it (tl_workload.c)

    The host loop is itself instrumented (host/frame > workload / ui / present, plus a
    host/wait zone around the pacer) so the timeline always has a real main-thread story to
    tell even with every workload scenario off.

    Drive it: watch live, drag/wheel into a frame, click a red bar in the frame strip to
    focus the spike, crank the spam slider and zoom in until the merged bars separate.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/prof/prof_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"
#include "sandbox/gui/sb_gui_timeline/gui_timeline.h"
#include "sandbox/gui/sb_gui_timeline/tl_workload.h"

// clang-format off

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( prof );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_timeline] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- prof timeline",
        .w     = 1280, .h = 760,
        .font  = GUI_FONT_JETBRAINS,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_timeline] gui->boot failed\n" );
        goto shutdown;
    }

    prof_thread_name( "main" );
    tl_workload_init();

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        prof_frame_mark();
        PROF_ZONE_BEGIN( "host/frame" );

        PROF_ZONE_BEGIN( "host/workload" );
        tl_workload_tick();
        PROF_ZONE_END();

        /* Drain after the workload so this frame's zones are already on screen. */
        gui_timeline_update();

        /* Live view mutates every frame; pin the emit so the clean-frame skip never stalls
           it. Paused view drops the pin and the ui idles like any static window. */
        gui()->set_force_redraw( gui_timeline_is_live() );

        PROF_ZONE_BEGIN( "host/ui" );
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            gui_timeline_window();
            tl_workload_window();
            gui()->ctx_end();
        }
        gui()->frame_end();
        PROF_ZONE_END();

        PROF_ZONE_BEGIN( "host/present" );
        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        PROF_ZONE_END();

        PROF_ZONE_END();    /* host/frame -- pacing wait tracked separately below */

        PROF_ZONE_BEGIN( "host/wait" );
        gui()->boot_pace ( 4, 16 );
        PROF_ZONE_END();
    }

    ret_code = 0;

shutdown:
    tl_workload_exit();
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
// clang-format on
