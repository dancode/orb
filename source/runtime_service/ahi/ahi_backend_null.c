/*==============================================================================================

    runtime_service/ahi/ahi_backend_null.c -- deviceless pacing backend (non-Windows).

    Keeps the mixer machinery real on platforms with no audio backend yet: a thread
    pulls mixed frames at device cadence and discards them, so voices start, advance,
    and finish exactly as they would against hardware.  Replace with an ALSA/CoreAudio
    backend when a POSIX audio path lands.

==============================================================================================*/

#define AHI_NULL_BLOCK_FRAMES 512    /* ~10.6 ms at 48 kHz */

static volatile i32 s_ahi_run;
static thread_t     s_ahi_thread;
static ahi_mix_fn   s_ahi_mix_cb;

static void
ahi_null_thread( void* arg )
{
    UNUSED( arg );
    static f32 sink[ AHI_NULL_BLOCK_FRAMES * AHI_CHANNELS ];

    while ( sys_atomic_read( &s_ahi_run ) )
    {
        s_ahi_mix_cb( sink, AHI_NULL_BLOCK_FRAMES );
        thread_sleep_ms( AHI_NULL_BLOCK_FRAMES * 1000 / AHI_SAMPLE_RATE );
    }
}

static bool
ahi_backend_start( ahi_mix_fn mix )
{
    s_ahi_mix_cb = mix;
    sys_atomic_write( &s_ahi_run, 1 );
    s_ahi_thread = thread_create( ahi_null_thread, NULL, 0 );
    return true;
}

static void
ahi_backend_stop( void )
{
    if ( !sys_atomic_read( &s_ahi_run ) )
        return;
    sys_atomic_write( &s_ahi_run, 0 );
    thread_join( s_ahi_thread );
}

/*============================================================================================*/
