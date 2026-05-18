#ifndef AUDIO_H
#define AUDIO_H
/*==============================================================================================

    audio.h — Public API exported by the audio module.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct audio_api_s
{
    int  ( *play )( const char* name, float volume ); /* returns handle or -1 */
    void ( *stop )( int handle );
    void ( *set_master_volume )( float volume );

} audio_api_t;

#if defined( BUILD_STATIC ) || defined( AUDIO_STATIC )
MOD_GATEWAY_STATIC( audio_api_t, audio )
#else
MOD_GATEWAY_DYNAMIC( audio_api_t, audio )
#endif

#if defined( BUILD_STATIC ) || defined( AUDIO_STATIC )
    #define MOD_USE_AUDIO    /* static build */
    #define MOD_FETCH_AUDIO  true
#else
    #define MOD_USE_AUDIO    MOD_DEFINE_API_PTR( audio_api_t, audio )
    #define MOD_FETCH_AUDIO  MOD_FETCH_API( audio_api_t, audio )
#endif

/*============================================================================================*/
#endif    // AUDIO_H
