#ifndef RUNTIME_H
#define RUNTIME_H
/*==============================================================================================

    runtime/runtime.h — Runtime module API and frame clock.

    Self-contained: include this directly from any module that needs run clock access.

    The run module is always statically linked into the host executable.
    It owns the authoritative frame clock and exposes it so any module can
    opt in to richer timing beyond the f32 dt passed to update callbacks.

    Usage in a module:
        MOD_DEFINE_API_PTR( run_api_t, run );

        // In init() / reload():
        if ( !MOD_FETCH_API( run_api_t, run ) ) return false;

        // At any call site:
        f64 t = run_api()->clock()->app_time;
        u64 f = run_api()->clock()->frame_number;

    run_clock_update() is host-internal — only host_main.c calls it once per frame
    before dispatching on_update. Modules must not call it.
==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"

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

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct run_api_s
{
    const run_clock_t* ( *clock )( void ); /* current frame clock (read-only)     */
    void ( *set_time_scale )( f32 );       /* adjust time scale from game or host */

} run_api_t;

#if defined( BUILD_STATIC ) || defined( RUN_STATIC )
MOD_GATEWAY_STATIC( run_api_t, run )
#else
MOD_GATEWAY_DYNAMIC( run_api_t, run )
#endif

/*==============================================================================================
    Host-internal — called once per frame by run_host.c before on_update is dispatched.
    Modules must not call this.
==============================================================================================*/

void run_clock_update( f64 app_time, f32 dt_real );

/*============================================================================================*/
#endif    // RUNTIME_H
