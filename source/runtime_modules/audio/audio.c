/*==============================================================================================

    audio.c

    Audio module — statically linked host module.

    This module is always compiled into the exe.  Dynamic DLLs that depend on audio
    call sys->get_api("audio") in their init() to receive a pointer to g_audio_api_struct.
    In static builds, callers who include audio_api.h resolve audio() directly to
    &g_audio_api_struct at link time — no pointer indirection, LTO-visible.

    PROVIDER PATTERN:
    ─────────────────
    1.  Implement the API functions as internal statics.
    2.  Define g_<name>_api_struct as a non-static global const — its address is the
        stable identity used by both the module system (func_api) and the gateway macro.
    3.  Define g_<module>_api (mod_desc_t) with func_api = &g_audio_api_struct.
    4.  Expose name_get_mod_desc() and name_get_api() for the module_load() macro.
    5.  Use MOD_DEFINE_EXPORTS to emit undecorated DLL symbols (no-op for static builds).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "engine/mod/mod_import.h"
#include "audio.h"

/*==============================================================================================
    Audio State
==============================================================================================*/

#define AUDIO_MAX_SOUNDS 32

typedef struct audio_state_s
{
    float master_volume;
    int   next_handle;

    struct
    {
        int  handle;
        char name[ 48 ];
        bool active;
    } sounds[ AUDIO_MAX_SOUNDS ];

} audio_state_t;

/* The system owns the state block and passes it into every callback.
   Cache it at init time so the API functions can reach it without an argument. */
static audio_state_t* s = NULL;

/*==============================================================================================
    API implementation
==============================================================================================*/

static int
audio_play( const char* name, float volume )
{
    for ( int i = 0; i < AUDIO_MAX_SOUNDS; ++i )
    {
        if ( s->sounds[ i ].active )
            continue;

        int h                 = s->next_handle++;
        s->sounds[ i ].handle = h;
        s->sounds[ i ].active = true;
        strncpy( s->sounds[ i ].name, name, sizeof( s->sounds[ i ].name ) - 1 );

        printf( "[audio] play '%s' vol=%.2f  handle=%d\n", name, volume * s->master_volume, h );
        return h;
    }
    printf( "[audio] WARN: no free sound slots\n" );
    return -1;
}

static void
audio_stop( int handle )
{
    for ( int i = 0; i < AUDIO_MAX_SOUNDS; ++i )
    {
        if ( s->sounds[ i ].active && s->sounds[ i ].handle == handle )
        {
            printf( "[audio] stop handle=%d ('%s')\n", handle, s->sounds[ i ].name );
            s->sounds[ i ].active = false;
            return;
        }
    }
}

static void
audio_set_master_volume( float volume )
{
    s->master_volume = volume;
    printf( "[audio] master volume → %.2f\n", volume );
}

/*============================================================================================*/

#include "audio_api.c"

/*============================================================================================*/