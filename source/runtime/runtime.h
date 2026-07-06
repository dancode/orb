#ifndef RUNTIME_H
#define RUNTIME_H
/*==============================================================================================

    runtime/runtime.h — Runtime module types.

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Frame clock
==============================================================================================*/

typedef struct run_clock_s
{
    f64 app_time;     /* seconds since engine start — monotonic, never reset       */
    u64 app_time_us;  /* same clock as integer microseconds -- the internal tick;
                         exact diffs for pacing, profiling, fixed-step accumulators */
    f32 dt;           /* capped, time-scaled delta — what game logic should consume */
    f32 dt_real;      /* raw uncapped delta — for profiling and diagnostics         */
    f32 time_scale;   /* multiplier on dt (1.0 = realtime, 0.0 = paused)           */
    u64 frame_number; /* 0-based monotonic counter — 0 on the first frame          */

} run_clock_t;

/*==============================================================================================
    Frame stats -- per-phase host loop timings in integer microseconds, refreshed every
    frame by the host after pacing.  Phases cover the named loop sections; small
    unattributed work (hot-reload checks, loop overhead) shows up in work_us only.
==============================================================================================*/

typedef struct run_frame_stats_s
{
    i64 events_us; /* OS pump + event drain                                  */
    i64 update_us; /* console poll + cmd pump + job tick + on_update         */
    i64 gui_us;    /* gui emit (frame_begin..frame_end) + viewport sync      */
    i64 render_us; /* main-surface render + floater presents                 */
    i64 work_us;   /* whole frame minus pacing                               */
    i64 wait_us;   /* pacing sleep / editor event wait                       */
    i64 frame_us;  /* full frame period: work_us + wait_us                   */

} run_frame_stats_t;

/*============================================================================================*/
#endif    // RUNTIME_H
