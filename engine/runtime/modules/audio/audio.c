/*==============================================================================================

    audio.c

    Audio module — statically linked host module.

    This module is always compiled into the exe.  Dynamic DLLs that depend on audio
    call sys->get_api("audio") in their init() to receive a pointer to g_audio_api_struct.
    In static builds, callers who include audio_api.h resolve audio_api() directly to
    &g_audio_api_struct at link time — no pointer indirection, LTO-visible.

    PROVIDER PATTERN:
    ─────────────────
    1.  Implement the API functions as internal statics.
    2.  Define g_<name>_api_struct as a non-static global const — its address is the
        stable identity used by both the module system (func_api) and the gateway macro.
    3.  Define g_<module>_api (module_api_t) with func_api = &g_audio_api_struct.
    4.  Expose name_get_module_api() and name_get_api() for the module_load() macro.
    5.  Use MODULE_DEFINE_EXPORTS to emit undecorated DLL symbols (no-op for static builds).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "module/module_api.h"
#include "audio_api.h"

/*==============================================================================================
    Persistent state — zeroed on first load, preserved across hot-reloads.
==============================================================================================*/

#define AUDIO_MAX_SOUNDS 32

typedef struct
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

/*==============================================================================================
    API struct — globally visible, non-static.

    In static builds: address known at link time, LTO can devirtualize through it.
    In dynamic builds: address is passed as func_api and returned from get_api(),
                       then cached by consumers as g_audio_api_ptr.

    Naming: g_<module>_api_struct matches the symbol the MODULE_GATEWAY macro declares
    extern in audio_api.h.  This is the convention — it prevents linker collisions when
    multiple modules are linked together in a monolithic build.
==============================================================================================*/

const audio_api_t g_audio_api_struct = {
    .play              = audio_play,
    .stop              = audio_stop,
    .set_master_volume = audio_set_master_volume,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
audio_init( void* state, get_api_fn get_api )
{
    ( void )get_api;
    s = ( audio_state_t* )state;

    /* State is zeroed on first call; master_volume of 0 means uninitialised. */
    if ( s->master_volume == 0.0f )
        s->master_volume = 1.0f;

    printf( "[audio] init  master_volume=%.2f\n", s->master_volume );
    return true;
}

static void
audio_tick( void* state, float dt )
{
    ( void )state;
    ( void )dt;
    /* submit audio buffer to OS driver */
}

static void
audio_exit( void* state )
{
    audio_state_t* a = ( audio_state_t* )state;
    for ( int i = 0; i < AUDIO_MAX_SOUNDS; ++i ) a->sounds[ i ].active = false;
    printf( "[audio] exit\n" );
}

static void
audio_on_reload( void* state, get_api_fn get_api )
{
    /* Re-anchor the local state pointer; no sibling APIs to re-cache. */
    s = ( audio_state_t* )state;
    printf( "[audio] reloaded  master_volume=%.2f\n", s->master_volume );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

module_api_t*
audio_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = sizeof( audio_state_t ),
        .deps       = { "core" },
        .dep_count  = 1,
        .func_api   = &g_audio_api_struct, /* the globally-visible struct above */
        .init       = audio_init,
        .tick       = audio_tick,
        .exit       = audio_exit,
        .on_reload  = audio_on_reload,
    };
    return &api;
};

void*
audio_get_api( void )
{
    return ( void* )&g_audio_api_struct;
}

/* Emits undecorated DLL exports in dynamic builds; expands to nothing in static builds. */
MODULE_DEFINE_EXPORTS( audio )
