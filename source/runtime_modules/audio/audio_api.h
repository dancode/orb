#ifndef AUDIO_API_H
#define AUDIO_API_H
/*==============================================================================================

    runtime_modules/audio/audio_api.h -- audio module API struct and gateway macro.

==============================================================================================*/

#include "runtime_modules/audio/audio.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct audio_api_s
{
    /* Add a sound to the bank.  The name is copied; sound->frames is CALLER-OWNED and
       must outlive playback (the bank survives hot-reloads in module state).
       Re-registering a name replaces its PCM.  False when the bank or name is full. */
    bool ( *sound_register )( const char* name, const ahi_sound_t* sound );

    /* Fire-and-forget by name.  volume is linear gain (1.0 = as authored).
       Returns AUDIO_HANDLE_INVALID for unknown names or when no voice is free. */
    audio_handle_t ( *play )( const char* name, f32 volume );

    /* Full-control variant: pan -1..+1, pitch = rate multiplier, loop until stop(). */
    audio_handle_t ( *play_ex )( const char* name, f32 volume, f32 pan, f32 pitch, bool loop );

    void ( *stop )( audio_handle_t h );            /* stale handles are ignored */
    bool ( *playing )( audio_handle_t h );

    void ( *master_volume )( f32 volume );

} audio_api_t;

#if defined( BUILD_STATIC ) || defined( AUDIO_STATIC )
MOD_GATEWAY_STATIC( audio_api_t, audio )
    #define MOD_USE_AUDIO    /* static build */
    #define MOD_FETCH_AUDIO  true
#else
MOD_GATEWAY_DYNAMIC( audio_api_t, audio )
    #define MOD_USE_AUDIO    MOD_DEFINE_API_PTR( audio_api_t, audio )
    #define MOD_FETCH_AUDIO  MOD_FETCH_API( audio_api_t, audio )
#endif

// clang-format on
/*============================================================================================*/
#endif    // AUDIO_API_H
