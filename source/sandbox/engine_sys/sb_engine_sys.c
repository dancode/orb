/*==============================================================================================

    sandbox_engine_sys.c - Test system module.

    Not a real host; just a place to call app APIs and verify they work.

    Boots the module system, loads the example module (static or dynamic per build mode),
    and runs a main loop exercising hot-reload, failure/rollback, and cached API access.


==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/sys/sys.h"

// #include "engine/sys/sys_api.h"

// #include "runtime_module/example/example_api.h"
// MOD_DEFINE_API_PTR( example_api_t, example );


// This can now live in your main game/app logic, completely independent of Windows.

typedef struct
{
    i64 last_usec;    // Store microseconds instead of OS-specific ticks
    f64 delta_seconds;
    f64 total_seconds;
    u64 frame_count;
} frame_timer_t;

void
frame_timer_init( frame_timer_t* timer )
{
    timer->last_usec     = sys_tick_microseconds();    // Use your abstract function
    timer->delta_seconds = 0.0;
    timer->total_seconds = 0.0;
    timer->frame_count   = 0;
}

void
frame_timer_update( frame_timer_t* timer, f64 max_delta )
{
    i64 now_usec     = sys_tick_microseconds();    // Use your abstract function
    i64 diff_usec    = now_usec - timer->last_usec;
    timer->last_usec = now_usec;

    // Convert microseconds to seconds for game logic (1 million us = 1 sec)
    timer->delta_seconds = ( f64 )diff_usec / 1000000.0;

    // Clamp to prevent the "spiral of death" during lag spikes
    if ( timer->delta_seconds > max_delta )
    {
        timer->delta_seconds = max_delta;
    }

    timer->total_seconds += timer->delta_seconds;
    timer->frame_count++;
}

/*============================================================================================*/

void
sys_test( void )
{
    // 1. Initialize the system frequency
    sys_tick_init();

    // 2. Initialize the frame timer
    frame_timer_t game_timer;
    frame_timer_init( &game_timer );

    // 3. Define game state
    f64  player_x     = 0.0;
    f64  player_speed = 50.0;    // Move 50 units per second
    bool running      = true;

    while ( running )
    {
        // 4. Update the timer at the very beginning of the frame.
        // We pass 0.1 (100ms) as the max_delta to prevent the spiral of death.
        frame_timer_update( &game_timer, 0.1 );

        // 5. Update game logic using the safe delta_seconds
        player_x += player_speed * game_timer.delta_seconds;

        // Render the game (Mock)
        printf( "Frame: %llu | Delta: %.4fs | Player X: %.2f\n", game_timer.frame_count,
                game_timer.delta_seconds, player_x );

        // Exit condition for this example
        if ( game_timer.total_seconds >= 2.0 )
        {
            running = false;
        }

        sys_sleep_milliseconds( 16 );    // Simulate ~60 FPS (16ms per frame)
    }
}

/*============================================================================================*/
/* main entry point */

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    sys_test();
    return 0;
}

/*============================================================================================*/