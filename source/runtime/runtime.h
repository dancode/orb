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
    f32 dt;           /* capped, time-scaled delta — what game logic should consume */
    f32 dt_real;      /* raw uncapped delta — for profiling and diagnostics         */
    f32 time_scale;   /* multiplier on dt (1.0 = realtime, 0.0 = paused)           */
    u64 frame_number; /* 0-based monotonic counter — 0 on the first frame          */

} run_clock_t;

/*============================================================================================*/
#endif    // RUNTIME_H
