/*==============================================================================================

    app_module.c : windowing and applicaiton lifecycle service

    app depends on core (for log/alloc) and on 
    sys directly for its OS calls
    (sys is statically linked, never registered in the module system).

==============================================================================================*/

#include "orb.h"

#include "engine/app/app_api.h"
#include "engine/app/app.h"

#include "engine/sys/sys.h"

/* we may need this later but for now no dependency on core */

// #include "engine/core/core_api.h"
// MOD_DEFINE_API_PTR( core_api_t, core );

/*==============================================================================================
    unity build
==============================================================================================*/

#include "engine/app/app_api.c"

/*============================================================================================*/
/* Naive fixed - timestep loop with optional FPS throttle. */

void
app_loop_run( const app_loop_t* loop )
{
    uint64_t prev_ms   = sys_tick_milliseconds();
    uint64_t target_ms = loop->target_fps > 0 ? ( 1000u / ( unsigned )loop->target_fps ) : 0u;

    for ( ;; )
    {
        uint64_t now_ms = sys_tick_milliseconds();
        float    dt_s   = ( float )( now_ms - prev_ms ) / 1000.0f;
        prev_ms         = now_ms;

        if ( !loop->on_frame( loop->user, dt_s ) )
            break;

        /* throttle */
        if ( target_ms > 0 )
        {
            uint64_t elapsed = sys_tick_milliseconds() - now_ms;
            if ( elapsed < target_ms )
                sys_sleep_milliseconds( ( int )( target_ms - elapsed ) );
        }
    }
}

/*============================================================================================*/




/*============================================================================================*/