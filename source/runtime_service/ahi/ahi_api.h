/*==============================================================================================

    runtime_service/ahi/ahi_api.h -- AHI service API struct and gateway macro.

    Include this in DLL .c files that drive audio through the ahi() vtable.  Host
    executables and sandboxes include ahi_host.h instead.

    All functions are MAIN-THREAD ONLY and non-blocking: they post commands to the mixer's
    lock-free ring and return.  A stopped/finished voice handle is safe to pass anywhere;
    stale handles are ignored (playing() reports false).

==============================================================================================*/
#ifndef AHI_API_H
#define AHI_API_H

#include "runtime_service/ahi/ahi.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct ahi_api_s
{
    /* Start a voice.  `sound` is copied by value; sound->frames is caller-owned and must
       stay valid until the voice stops.  `params` may be NULL for ahi_params_default().
       Returns AHI_VOICE_INVALID when the device is absent or all voices are busy. */
    ahi_voice_t ( *play )( const ahi_sound_t* sound, const ahi_params_t* params );

    void ( *voice_stop      )( ahi_voice_t v );
    bool ( *voice_playing   )( ahi_voice_t v );
    void ( *voice_set_gain  )( ahi_voice_t v, f32 gain );
    void ( *voice_set_pan   )( ahi_voice_t v, f32 pan );      /* -1 .. +1 */
    void ( *voice_set_pitch )( ahi_voice_t v, f32 pitch );

    void ( *bus_set_gain    )( u32 bus, f32 gain );           /* ahi_bus_t */
    void ( *master_set_gain )( f32 gain );
    void ( *stop_all        )( void );

    u32  ( *sample_rate     )( void );    /* device output rate; 0 = no device (silent mode) */
    u32  ( *voice_count     )( void );    /* voices currently live (pending + playing) */

} ahi_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( AHI_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
MOD_GATEWAY_STATIC( ahi_api_t, ahi )
    #define MOD_USE_AHI     /* static: gateway returns pointer to global struct directly */
    #define MOD_FETCH_AHI   true
#else
MOD_GATEWAY_DYNAMIC( ahi_api_t, ahi )
    #define MOD_USE_AHI     MOD_DEFINE_API_PTR( ahi_api_t, ahi )
    #define MOD_FETCH_AHI   MOD_FETCH_API( ahi_api_t, ahi )
#endif

// clang-format on
/*============================================================================================*/
#endif    // AHI_API_H
