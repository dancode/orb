/*==============================================================================================

    sandbox/gui/sb_gui_bench/sb_gui_bench.c -- the gui performance benchmark suite.

    Measures the pipeline, not correctness: each case is one steady-state workload held for a
    fixed frame budget while the host samples emit time (its own clock bracket around
    frame_begin..frame_end) and gui()->render_stats() (diff / tess / submit CPU zones, the GPU
    frame timer, geometry + upload counts).  Suites:

      pipeline  one stage each: widget-emit walls, the diff-vs-retess static pair (their
                difference is the retained cache's ROI), and an all-windows-animated frame
                that prices upload + submit.
      text      one glyph workload under four draw paths (plain / clipped / rotated / outline).
      fill      N-layer flat overdraw -- the raw fragment fill rate, and the noise floor the
                op deltas are read against.
      op        the shader-op matrix: the same cell grid under every field / paint / pattern /
                repeat / clock op, gpu_ms delta vs the flat fill = that op's per-pixel price.
      style     one composite scene under the built-in themes and under synthesized variants
                (shadows off, rounding forced, borders off...) -- does a simpler style save.

    Command line:
      -run           run the whole suite unattended, write the report, exit.
      -case <s>      scripted run of the cases whose name contains <s> (or suite == <s>).
      -list          print every case and exit.
      -frames <n>    measured frames per case (default 120, cap 1024).
      -settle <n>    settle frames per case (default 30).
      (no args)      interactive: a picker window loops any case live for eyeballing; the
                     scripted run is the one that measures.

    Results land in artifacts/bench/gui_bench_YYYYMMDD_HHMMSS.txt (kept output, one file per
    run) and mirror to stdout.  NOTHING IS CACHED ACROSS MEASURED FRAMES BY DEFAULT: scripted
    runs pin force_redraw (the clean-frame skip never engages) and run every case with the
    retained geometry cache OFF, so emit, diff, tessellation, upload, submit and the GPU all
    run fresh every frame -- diff_static_scene is the one deliberate exception that measures
    the retained replay path itself.  Scripted runs also boot with debug OFF so the internal
    selector menu can never fight those levers, pin DPI at manual 1.0 so runs compare across
    monitors, and free-run (no pacing) so CPU columns are not floored at the refresh interval.
    Only same-build-config runs are comparable; the report header records the config.

==============================================================================================*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

#include "bench.h"
#include "bench_scenes.c"
#include "bench_pipeline.c"
#include "bench_gpu.c"
#include "bench_style.c"
#include "bench_core.c"

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    bool list = false;

    for ( int i = 1; i < argc; ++i )
    {
        if      ( strcmp( argv[ i ], "-run"  ) == 0 ) s_arg_run = true;
        else if ( strcmp( argv[ i ], "-list" ) == 0 ) list      = true;
        else if ( strcmp( argv[ i ], "-case" ) == 0 && i + 1 < argc )
        {
            s_arg_case = argv[ ++i ];
            s_arg_run  = true;
        }
        else if ( strcmp( argv[ i ], "-frames" ) == 0 && i + 1 < argc )
        {
            i32 n = atoi( argv[ ++i ] );
            if ( n < 1 )                 n = 1;
            if ( n > BENCH_MAX_SAMPLES ) n = BENCH_MAX_SAMPLES;
            s_arg_frames = ( u32 )n;
        }
        else if ( strcmp( argv[ i ], "-settle" ) == 0 && i + 1 < argc )
        {
            i32 n = atoi( argv[ ++i ] );
            if ( n < 1 ) n = 1;
            s_arg_settle = ( u32 )n;
        }
    }

    if ( list )
    {
        bench_list_print();
        return 0;
    }

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_bench] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* Scripted runs boot with debug OFF: the armed selector menu writes the force-redraw and
       retained-cache levers every frame and would fight the harness. */
    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui bench",
        .x     = 32, .y = 32,  
        .w     = ( i32 )BENCH_HOST_W, .h = ( i32 )BENCH_HOST_H,
        .font  = GUI_FONT_JETBRAINS,
        .clock = sys_tick_seconds,          /* arms the diff/tess/submit zones */
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
        .debug = !s_arg_run,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_bench] gui->boot failed\n" );
        goto shutdown;
    }

    bench_assets_init();

    if ( s_arg_run )
    {
        /* Pin the measured conditions: every frame emits (an idle-skipped frame records no
           stats at all), and DPI is manual 1.0 so two machines' runs compare. */
        gui()->dpi_set( GUI_DPI_MANUAL, 1.0f );
        gui()->set_force_redraw( true );
        if ( !bench_run_start() )
            goto shutdown;
    }
    else
    {
        gui()->debug_enable( true );
    }

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        s_dt_avg += ( dt - s_dt_avg ) * 0.05f;

        /* The emit bracket: frame_begin through frame_end is the widget-layer cost, the same
           seam the internal perf overlay times. */
        f64 t_emit = sys_tick_seconds();
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            bench_frame();
            gui()->ctx_end();
        }
        gui()->frame_end();
        f64 emit_ms = ( sys_tick_seconds() - t_emit ) * 1000.0;

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        gui()->boot_pace( 0, 0 );   /* free-run: pacing would floor every CPU column */

        if ( s_arg_run && !bench_tick( emit_ms ) )
            break;
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
// clang-format on
