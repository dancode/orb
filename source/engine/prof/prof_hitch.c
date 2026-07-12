/*==============================================================================================

    engine/prof/prof_hitch.c - Hitch capture: rolling history + automatic trace dump.

    An armed hitch monitor makes the profiler a flight recorder: prof_hitch_update runs
    once per frame as THE single drain consumer, folding every ring's events into fixed
    per-thread history buffers that always hold the most recent past. When the caller
    reports a frame time over the armed threshold, the history is replayed through the
    Chrome-trace writer (prof_dump.c) into an auto-named file -- the frames LEADING UP TO
    and INCLUDING the hitch, captured after the fact with nothing running in advance.

    Consumer-side only, same rule as the dump: one thread calls arm/update, and while an
    explicit prof_dump capture is active the monitor stands down (update is a no-op) so
    there is never a second drainer. Whole feature is gated by ORB_PROFILE_HITCH; at 0
    the history storage disappears and the entry points compile to inert stubs.

    Included by prof.c (unity) after prof_dump.c: reuses its writer and scratch buffer.

==============================================================================================*/

/* INFO: Why hitch capture needs its own history buffers.

   The naive plan -- "don't drain, and when a hitch happens dump whatever the rings hold"
   -- fails because the rings drop NEWEST on overflow (see prof.c): with no consumer they
   fill up with the oldest events and then reject everything, so the hitch frame itself is
   exactly what would be missing. The monitor therefore drains every frame (keeping the
   rings near-empty and drop-free) and re-buffers into its own circular history, which
   overwrites OLDEST -- safe here because one thread owns both cursors, no concurrency.
   Ring = producer-safe funnel, history = recent-past reservoir; each side overflows in
   the direction its role demands.                                                         */

#if ORB_PROFILE_HITCH

// clang-format off

_Static_assert( ( PROF_HITCH_HISTORY_CAP & ( PROF_HITCH_HISTORY_CAP - 1 ) ) == 0,
                "hitch history cap must be a power of two" );

typedef struct prof_hitch_s
{
    f64  threshold_ms;                  // > 0 = armed
    u32  cooldown;                      // frames left before the next capture may fire
    u32  count;                         // captures written since armed
    u32  head[ PROF_MAX_THREADS ];      // free-running append cursor per thread history
    char prefix[ 96 ];                  // capture filename prefix
    char last_path[ 160 ];              // filename of the most recent capture

} prof_hitch_t;

static prof_hitch_t g_prof_hitch;
static prof_event_t g_prof_hitch_hist[ PROF_MAX_THREADS ][ PROF_HITCH_HISTORY_CAP ];

// clang-format on

/* Append a drained batch to one thread's circular history -- overwrite-oldest is fine
   here: the monitor thread is the only reader AND the only writer of these buffers. */
static void
prof_hitch_append( u32 t, const prof_event_t* ev, u32 n )
{
    u32 h = g_prof_hitch.head[ t ];
    for ( u32 i = 0; i < n; ++i )
        g_prof_hitch_hist[ t ][ ( h + i ) & ( PROF_HITCH_HISTORY_CAP - 1 ) ] = ev[ i ];
    g_prof_hitch.head[ t ] = h + n;
}

/* Write the buffered history as a Chrome trace via the dump writer, then reset the
   history so the next capture window starts fresh (no duplicated events across files). */
static u32
prof_hitch_write( void )
{
    snprintf( g_prof_hitch.last_path, sizeof( g_prof_hitch.last_path ), "%s_%llu.json",
              g_prof_hitch.prefix, ( unsigned long long )prof_frame_number() );

    if ( !prof_dump_begin( g_prof_hitch.last_path ) )
    {
        g_prof_hitch.last_path[ 0 ] = 0;
        return 0;
    }

    u32 written = 0;
    u32 threads = prof_thread_count();

    for ( u32 t = 0; t < threads; ++t )
    {
        u32 h = g_prof_hitch.head[ t ];
        u32 n = h < PROF_HITCH_HISTORY_CAP ? h : PROF_HITCH_HISTORY_CAP;
        for ( u32 i = h - n; i != h; ++i )
            prof_dump_event( t, &g_prof_hitch_hist[ t ][ i & ( PROF_HITCH_HISTORY_CAP - 1 ) ] );
        written += n;
    }

    /* Live tail: anything other threads emitted since this frame's drain, plus the
       counter samples; dump_end then adds thread metadata and closes the file. */
    written += prof_dump_flush();
    prof_dump_end();

    memset( g_prof_hitch.head, 0, sizeof( g_prof_hitch.head ) );
    g_prof_hitch.count++;
    return written;
}

/*==============================================================================================
    Public surface
==============================================================================================*/

/* Arm the monitor (threshold_ms > 0) or disarm it (<= 0). Arming starts a fresh session:
   history, cooldown, capture count, and last path all reset. NULL/empty prefix -> "hitch";
   captures land in "<prefix>_<frame>.json". */
void
prof_hitch_arm( f64 threshold_ms, const char* path_prefix )
{
    memset( &g_prof_hitch, 0, sizeof( g_prof_hitch ) );
    g_prof_hitch.threshold_ms = threshold_ms > 0.0 ? threshold_ms : 0.0;
    snprintf( g_prof_hitch.prefix, sizeof( g_prof_hitch.prefix ), "%s",
              path_prefix && path_prefix[ 0 ] ? path_prefix : "hitch" );
}

bool
prof_hitch_armed( void )
{
    return g_prof_hitch.threshold_ms > 0.0;
}

u32
prof_hitch_count( void )
{
    return g_prof_hitch.count;
}

const char*
prof_hitch_last_path( void )
{
    return g_prof_hitch.last_path[ 0 ] ? g_prof_hitch.last_path : NULL;
}

/* INFO: Why the caller supplies frame_ms instead of the monitor timing itself.

   The monitor could difference its own clock reads between updates, but the HOST knows
   which portion of the frame is honest work: an editor host deliberately blocks on OS
   input for whole seconds between frames, and a paced game host sleeps off its budget
   surplus -- neither is a hitch. Passing the host's own work-time measure keeps that
   policy where the knowledge lives; the monitor just compares against the threshold.     */

/* Per-frame monitor tick -- THE drain consumer while armed. Folds this frame's events
   into the rolling history; when frame_ms is at/over the threshold (and the post-capture
   cooldown has lapsed) writes the history as a trace file. Returns the events written
   (0 = no capture fired). Inert while disarmed or while an explicit dump is active. */
u32
prof_hitch_update( f64 frame_ms )
{
    if ( g_prof_hitch.threshold_ms <= 0.0 )
        return 0;
    if ( prof_dump_active() )
        return 0;

    u32 threads = prof_thread_count();
    for ( u32 t = 0; t < threads; ++t )
    {
        u32 n;
        while ( ( n = prof_drain( t, g_prof_dump_buf, PROF_DUMP_CHUNK ) ) != 0 )
            prof_hitch_append( t, g_prof_dump_buf, n );
    }

    if ( g_prof_hitch.cooldown )
    {
        g_prof_hitch.cooldown--;
        return 0;
    }

    if ( frame_ms < g_prof_hitch.threshold_ms )
        return 0;

    g_prof_hitch.cooldown = PROF_HITCH_COOLDOWN_FRAMES;
    return prof_hitch_write();
}

#else /* ORB_PROFILE_HITCH == 0 -- inert stubs; the vtable shape never changes */

// clang-format off

void        prof_hitch_arm       ( f64 threshold_ms, const char* path_prefix ) { UNUSED( threshold_ms ); UNUSED( path_prefix ); }
bool        prof_hitch_armed     ( void )           { return false; }
u32         prof_hitch_count     ( void )           { return 0; }
const char* prof_hitch_last_path ( void )           { return NULL; }
u32         prof_hitch_update    ( f64 frame_ms )   { UNUSED( frame_ms ); return 0; }

// clang-format on

#endif    /* ORB_PROFILE_HITCH */

/*============================================================================================*/
