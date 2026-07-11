/*==============================================================================================

    runtime_service/ahi/ahi_backend.h -- device backend seam (internal to the ahi unity).

    A backend owns the OS device and a dedicated audio thread.  It calls `mix` on that
    thread whenever the device wants frames: interleaved f32, AHI_CHANNELS wide, at
    AHI_SAMPLE_RATE (backends convert to the hardware format themselves).

    Exactly one backend is compiled into the unity: WASAPI on Windows, the null pacer
    elsewhere.  Backends must not log -- they run on the audio thread; they report
    startup failure through ahi_backend_start()'s return instead.

==============================================================================================*/
#ifndef AHI_BACKEND_H
#define AHI_BACKEND_H

typedef void ( *ahi_mix_fn )( f32* out_frames, u32 frame_count );

/* Spawn the audio thread and open the device; blocks until the device is up (or failed).
   Returns false when no output path exists -- the mixer then runs in silent mode. */
static bool ahi_backend_start( ahi_mix_fn mix );

/* Stop the stream, join the audio thread, release the device.  Safe when never started. */
static void ahi_backend_stop( void );

/*============================================================================================*/
#endif    // AHI_BACKEND_H
