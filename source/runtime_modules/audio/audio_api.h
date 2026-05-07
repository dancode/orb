#ifndef AUDIO_API_H
#define AUDIO_API_H
/*==============================================================================================

    audio_api.h

    Public API exported by the audio module.

==============================================================================================*/

#include "engine/mod/mod_api.h"

/*==============================================================================================
    Audio API struct
==============================================================================================*/

typedef struct audio_api_s
{
    int ( *play )( const char* name, float volume ); /* returns handle or -1 */
    void ( *stop )( int handle );
    void ( *set_master_volume )( float volume );

} audio_api_t;

/*==============================================================================================
    Gateway — one line generates the inline accessor for all consumers.
==============================================================================================*/

#if defined( BUILD_STATIC ) || defined( AUDIO_LINK_STATIC )
MOD_GATEWAY_STATIC( audio_api_t, audio )
#else
MOD_GATEWAY_DYNAMIC( audio_api_t, audio )
#endif

// MODULE_GATEWAY( audio_api_t, audio )

/*============================================================================================*/
#endif    // AUDIO_API_H
