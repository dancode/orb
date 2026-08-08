/*==============================================================================================

    sandbox/gui/sb_gui_timeline/tl_workload.c - synthetic capture workload for the timeline.

    Every scenario targets a specific thing the timeline must render well:

      nested tree    -- a depth-sliding chain of sim/<name> zones with breathing durations: bar
                        nesting, per-name colors, labels appearing as bars widen under zoom
      workers        -- a fixed pool of threads claimed up front (one prof ring each); the
                        slider sets how many are BUSY, the rest idle -- multi-track drawing
                        without ring churn
      micro spam     -- N back-to-back tiny zones per frame: the sub-pixel LOD path and ring
                        pressure (drops readout)
      spike          -- a one-shot or periodic 40 ms zone: the frame strip's red bar and the
                        click-to-focus flow
      counter + mem  -- tl/sin counter and tl/mem scope churn: exercised so dumps taken while
                        the sandbox runs carry them (the timeline itself draws zones)

    Workers busy-wait rather than sleep through their work so zones have real width; the
    work is a spin on the tick clock, so durations are exact and tunable.

==============================================================================================*/

#include <stdio.h>
#include <math.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/prof/prof_host.h"
#include "runtime_service/gui/gui_host.h"
#include "sandbox/gui/sb_gui_timeline/tl_workload.h"

// clang-format off

#define TL_MAX_WORKERS 4

typedef struct tl_worker_s
{
    thread_t     thread;
    u32          index;
    char         name[ 16 ];
} tl_worker_t;

/* Scenario toggles -- read by the tick + the workers, written by the control window. */
static bool         s_nested      = true;
static i32          s_nest_depth  = 4;       // zone chain depth, 1..6
static f32          s_nest_ms     = 2.0f;    // total busy time across the chain
static volatile i32 s_busy_workers = 2;      // workers doing work (rest idle)
static bool         s_spam        = false;
static i32          s_spam_count  = 300;
static bool         s_auto_spike  = false;
static bool         s_spike_once  = false;
static bool         s_telemetry   = true;    // counter + mem churn

static volatile i32 s_shutdown    = 0;
static tl_worker_t  s_workers[ TL_MAX_WORKERS ];
static u32          s_frame       = 0;
static i64          s_mem_live    = 0;       // bytes currently "allocated" on the tl/mem scope

// clang-format on

/*==============================================================================================
    Busy work
==============================================================================================*/

/* Spin until the tick clock passes the deadline -- exact-width zones, unlike a sleep. */
static void
tl_busy_ns( i64 ns )
{
    i64          end = sys_tick_nanoseconds() + ns;
    volatile u32 x   = 1;
    while ( sys_tick_nanoseconds() < end )
        x = x * 1664525u + 1013904223u;
}

/* Cheap per-caller PRNG for duration jitter. */
static u32
tl_rand( u32* state )
{
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

/*==============================================================================================
    Main-thread scenarios
==============================================================================================*/

/* A chain of named levels, each burning an equal share and recursing -- plus leaf siblings
   at the bottom so zoomed-in frames show adjacent bars, not just nesting. Zone names come
   from a table (the PROF_ZONE macros cache one id per CALL SITE, which a recursive function
   defeats), so this uses the direct name calls. */
static void
tl_nested_level( i32 depth, i32 max_depth, i64 level_ns )
{
    static const char* k_levels[] = { "sim/update", "sim/physics", "sim/anim",
                                      "sim/ai",     "sim/audio",   "sim/fx" };

    prof_zone_begin_name( k_levels[ depth ] );
    tl_busy_ns( level_ns / 2 );

    if ( depth + 1 < max_depth && depth + 1 < ( i32 )ARRAY_COUNT( k_levels ) )
        tl_nested_level( depth + 1, max_depth, level_ns );
    else
    {
        prof_zone_begin_name( "sim/leaf_a" );
        tl_busy_ns( level_ns / 4 );
        prof_zone_end();
        prof_zone_begin_name( "sim/leaf_b" );
        tl_busy_ns( level_ns / 4 );
        prof_zone_end();
    }

    tl_busy_ns( level_ns / 2 );
    prof_zone_end();
}

void
tl_workload_tick( void )
{
    s_frame++;

    PROF_ZONE_BEGIN( "tl/tick" );

    if ( s_nested )
    {
        /* Breathe the load so the frame strip undulates: 0.5x..1.5x over ~3 seconds. */
        f32 breathe = 1.0f + 0.5f * sinf( ( f32 )s_frame * 0.035f );
        i64 total   = ( i64 )( s_nest_ms * breathe * 1.0e6f );
        tl_nested_level( 0, s_nest_depth, total / ( s_nest_depth > 0 ? s_nest_depth : 1 ) );
    }

    if ( s_spam )
    {
        PROF_ZONE_BEGIN( "tl/spam_burst" );
        for ( i32 i = 0; i < s_spam_count; ++i )
        {
            prof_zone_begin_name( "tl/spam" );
            prof_zone_end();
        }
        PROF_ZONE_END();
    }

    if ( s_spike_once || ( s_auto_spike && ( s_frame % 180 ) == 0 ) )
    {
        s_spike_once = false;
        PROF_ZONE_BEGIN( "tl/spike" );
        tl_busy_ns( 40 * 1000 * 1000 );
        PROF_ZONE_END();
    }

    if ( s_telemetry )
    {
        PROF_COUNTER_SET( "tl/sin", ( i64 )( 1000.0f * ( 1.0f + sinf( ( f32 )s_frame * 0.05f ) ) ) );

        /* Saw-tooth mem churn on one scope: grow for 100 frames, release everything, repeat. */
        static u32 rng = 0x2F6E2B1u;
        i64        sz  = 256 + ( i64 )( tl_rand( &rng ) & 0x3FFF );
        PROF_MEM_ALLOC( "tl/mem", sz );
        s_mem_live += sz;
        if ( ( s_frame % 100 ) == 0 && s_mem_live > 0 )
        {
            PROF_MEM_FREE( "tl/mem", s_mem_live );
            s_mem_live = 0;
        }
    }

    PROF_ZONE_END();
}

/*==============================================================================================
    Workers -- a fixed pool claimed up front; the slider gates who works, nobody respawns
==============================================================================================*/

static void
tl_worker_main( void* arg )
{
    tl_worker_t* w = ( tl_worker_t* )arg;
    prof_thread_name( w->name );

    u32 rng = 0x9E3779B9u ^ ( w->index * 0x85EBCA6Bu + 1u );

    while ( !sys_atomic_read( &s_shutdown ) )
    {
        if ( ( i32 )w->index < sys_atomic_read( &s_busy_workers ) )
        {
            prof_zone_begin_name( "worker/job" );
            u32 steps = 2 + ( tl_rand( &rng ) & 3 );
            for ( u32 k = 0; k < steps; ++k )
            {
                prof_zone_begin_name( "worker/step" );
                tl_busy_ns( 200 * 1000 + ( i64 )( tl_rand( &rng ) % ( 900 * 1000 ) ) );
                prof_zone_end();
            }
            prof_zone_end();

            thread_sleep_ms( 3 + w->index * 2 + ( tl_rand( &rng ) & 7 ) );
        }
        else
            thread_sleep_ms( 20 );    /* parked: emits nothing, keeps its ring */
    }
}

void
tl_workload_init( void )
{
    for ( u32 i = 0; i < TL_MAX_WORKERS; ++i )
    {
        tl_worker_t* w = &s_workers[ i ];
        w->index       = i;
        snprintf( w->name, sizeof( w->name ), "worker %u", i );
        w->thread = thread_create( tl_worker_main, w, 0 );
    }
}

void
tl_workload_exit( void )
{
    sys_atomic_write( &s_shutdown, 1 );
    for ( u32 i = 0; i < TL_MAX_WORKERS; ++i )
        if ( thread_valid( s_workers[ i ].thread ) )
            thread_join( s_workers[ i ].thread );
}

/*==============================================================================================
    Control window
==============================================================================================*/

void
tl_workload_window( void )
{
    gui()->window_set_next_pos( 20.0f, 500.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 480.0f, 240.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Workload", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->field_label_left( 130.0f );

        gui()->checkbox( "Nested sim tree", &s_nested );
        gui()->slider_int( "depth", &s_nest_depth, 1, 6 );
        gui()->slider_float( "load ms", &s_nest_ms, 0.0f, 8.0f );
        gui()->separator();

        i32 busy = sys_atomic_read( &s_busy_workers );
        if ( gui()->slider_int( "busy workers", &busy, 0, TL_MAX_WORKERS ) )
            sys_atomic_write( &s_busy_workers, busy );
        gui()->separator();

        gui()->checkbox( "Micro-zone spam", &s_spam );
        gui()->slider_int( "zones/frame", &s_spam_count, 10, 2000 );
        gui()->separator();

        if ( gui()->button( "Spike now (40 ms)" ) )
            s_spike_once = true;
        gui()->checkbox( "Auto spike every 3 s", &s_auto_spike );
        gui()->separator();

        gui()->checkbox( "Counter + mem churn", &s_telemetry );
        gui()->textf( "frame %u   tl/mem live %lld bytes", s_frame, ( long long )s_mem_live );
    }
    gui()->window_end();
}

/*============================================================================================*/
