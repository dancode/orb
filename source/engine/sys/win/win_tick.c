/*==============================================================================================

    platform_win_tick.c

==============================================================================================*/
/*==============================================================================================



==============================================================================================*/

static i64  sys_tick_frequency;        // cached QPC ticks per second.
static i64  sys_tick_nano;             // cacehd QPC nanoseconds per tick.
static i64  sys_tick_start;            // time base for counting (offset timing from init start)
static bool sys_time_period_called;    // sync init and eixt

// clang-format off
/*==============================================================================================

    KHz = 1000
    MHz = 1000 * 1000 = 1,000,000 million
    GHz = 1000 * 1000 * 1000 per second = 1,000,000,000 billion

    0, 000 000 001   [ billionth ]      nanosecond [ ns ] = Ghz
    0, 000 001       [ millionth ]      microsecond[ us ] 1,000,000 = 1 second
    0, 001           [ thousandth ]     millisecond[ ms ] 1,000 = 1 second
    0.01             [ hundredth ]      centisecond[ cs ]
    1.0                                 second[ s ]

==============================================================================================*/

static void
sys_tick_get_frequency( void )
{
    // get the high resolution ticks per second from CPU performance counter clock.

    LARGE_INTEGER freq;
    if ( !QueryPerformanceFrequency( &freq ) )
    {
        exit( 1 );    // should be supported on all platforms.
    }
    sys_tick_frequency = freq.QuadPart;

    // for reference store the nanoseconds per a clock tick (100 on my pc).
    sys_tick_nano = 1000000000 / sys_tick_frequency;
}

/*============================================================================================*/

f64 /* elapsed since last reset */
sys_tick_reset( void )
{
    LARGE_INTEGER time;
    QueryPerformanceCounter( &time );

    // Reset time base of clock to zero.

    i64 diff        = time.QuadPart - sys_tick_start;
    f64 sec_elapsed = (f64)diff / (f64)sys_tick_frequency;
    sys_tick_start  = time.QuadPart;
    return sec_elapsed;
};

/*============================================================================================*/

void
sys_tick_init( void )
{
    // timeBeginPeriod() function requests a minimum resolution for periodic timers that
    // ensures Sleep() counts at the lowest resolution interval. 

    if ( sys_time_period_called == false ) {
         sys_time_period_called = true;
         timeBeginPeriod( 1 );
    }
    sys_tick_get_frequency();
    sys_tick_reset();
    sys_tick_reset();    // called twice to clear sys_tick_start to zero.
}

void
sys_tick_exit( void )
{
    // timeEndPeriod() must be called after every timeBeginPeriod().
    if ( sys_time_period_called == true ) {
         sys_time_period_called = false;
         timeEndPeriod( 1 );
    }
}

/*============================================================================================*/

f64
sys_tick_seconds( void )
{
    // Convert frequency (times per second) by performance counter.

    LARGE_INTEGER time;
    QueryPerformanceCounter( &time );

    i64 diff = time.QuadPart - sys_tick_start;
    f64 tick = (f64)diff / sys_tick_frequency;
    return tick;
}

/*============================================================================================*/

i64
sys_tick_milliseconds( void )
{
    // Convert to time elapsed that fits 1000 times into a second.

    LARGE_INTEGER time;
    QueryPerformanceCounter( &time );

    i64 diff = time.QuadPart - sys_tick_start;
    i64 tick = ( diff * 1000ull ) / sys_tick_frequency;
    return tick;
}

/*============================================================================================*/

i64
sys_tick_microseconds( void )
{
    // Convert gigahertz to microseconds (million per second) then divide by frequency,
    // to prevent loss-of-precision.

    LARGE_INTEGER time;
    QueryPerformanceCounter( &time );

    i64 diff = time.QuadPart - sys_tick_start;
    i64 tick = ( 1000000ull * diff ) / sys_tick_frequency;
    return tick;
}

/*============================================================================================*/

i64
sys_tick_nanoseconds( void )
{
    // Convert gigahertz to nanoseconds (billion per second). Since QPC is larger than
    // a single nanosecond we get 100 nanoseconds per tick (we must adjust)

    LARGE_INTEGER time;
    QueryPerformanceCounter( &time );

    i64 diff = time.QuadPart - sys_tick_start;
    i64 tick = diff * sys_tick_nano; /* 100 */
    return tick;
}

/*============================================================================================*/

void
sys_sleep_milliseconds( i32 milliseconds )
{
    /* Sleep() is not super accurate, but it's good enough for our needs.  We call timeBeginPeriod(1)
       in sys_tick_init to ensure that Sleep() has a resolution of 1 ms, which is sufficient for
       our frame throttling.  For more precise sleeping, we would need to implement a busy-wait
       loop that checks the high-resolution timer, but that would consume CPU resources. */

    Sleep( milliseconds ); /* win32 sleep function */
}

/*============================================================================================*/
// clang-format on

/*============================================================================================*/