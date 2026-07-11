/*==============================================================================================

    runtime_service/ahi/ahi.h -- Audio Hardware Interface, public types and handles.

    The audio analog of the RHI: a STATIC service that owns the output device and the
    mixer.  Everything above it (the hot-reloadable audio module, game sound code) only
    submits commands through the ahi() vtable; nothing above it ever touches the device
    or runs on the audio thread.

    Threading model
    ---------------
        The backend runs a dedicated audio thread that pulls mixed frames on the device's
        cadence.  All ahi() calls are made from the MAIN thread only; they communicate
        with the mixer through a lock-free single-producer/single-consumer command ring.
        Nothing in this API blocks on the audio thread.

    Sample data
    -----------
        Sounds are caller-owned f32 PCM: the frames pointer must stay valid until every
        voice playing it has stopped.  The mixer resamples (linear) from the sound's rate
        to the device rate, which is also how pitch works.

==============================================================================================*/
#ifndef AHI_H
#define AHI_H

#include "orb.h"

// clang-format off
/*==============================================================================================
    Device format  (fixed; the backend converts to whatever the hardware wants)
==============================================================================================*/

#define AHI_SAMPLE_RATE     48000
#define AHI_CHANNELS        2

/*==============================================================================================
    Voice handle

    Generation-checked id: slot in the low byte (+1 so 0 stays null), generation above.
    Voices finish asynchronously on the audio thread, so stale handles are the common
    case -- every call taking a handle quietly ignores a stale one.
==============================================================================================*/

typedef struct { u32 id; } ahi_voice_t;

#define AHI_VOICE_INVALID        0
#define ahi_voice_valid( v )     ( (v).id != AHI_VOICE_INVALID )

#define AHI_MAX_VOICES           64

/*==============================================================================================
    Buses  (per-category gain groups, all summed under the master gain)
==============================================================================================*/

typedef enum ahi_bus_e
{
    AHI_BUS_SFX = 0,
    AHI_BUS_MUSIC,
    AHI_BUS_UI,

    AHI_BUS_COUNT

} ahi_bus_t;

/*==============================================================================================
    Sound  (caller-owned PCM description; copied by value into the voice at play time)
==============================================================================================*/

typedef struct ahi_sound_s
{
    const f32* frames;          // interleaved f32 PCM, CALLER-OWNED, alive while playing
    u32        frame_count;     // frames, not samples
    u32        channels;        // 1 (mono) or 2 (stereo interleaved)
    u32        sample_rate;     // source rate; mixer resamples to AHI_SAMPLE_RATE

} ahi_sound_t;

/*==============================================================================================
    Play parameters  (pass NULL to play() for these defaults)
==============================================================================================*/

typedef struct ahi_params_s
{
    f32 gain;                   // linear amplitude, 1.0 = as authored
    f32 pan;                    // -1 full left .. +1 full right (constant power)
    f32 pitch;                  // playback rate multiplier, 1.0 = as authored
    u32 bus;                    // ahi_bus_t
    b32 loop;                   // wrap at the end instead of freeing the voice

} ahi_params_t;

static inline ahi_params_t
ahi_params_default( void )
{
    ahi_params_t p = { 1.0f, 0.0f, 1.0f, AHI_BUS_SFX, false };
    return p;
}

// clang-format on
/*============================================================================================*/
#endif    // AHI_H
