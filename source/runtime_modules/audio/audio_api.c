/*==============================================================================================

    audio_api.c -- audio module wiring.
    Implements the audio_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers

    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),
    then fetch in init() and reload() with MOD_FETCH_<NAME>:

        MOD_USE_CORE;                                    // file scope
        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()
==============================================================================================*/

/* none */

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
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

static audio_state_t* s = NULL;

/*==============================================================================================
    Implementation
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
    printf( "[audio] master volume -> %.2f\n", volume );
}

/*==============================================================================================
    API Struct
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
audio_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s = ( audio_state_t* )raw_state;

    /* state is zeroed on first call; master_volume of 0 means uninitialised */
    if ( s->master_volume == 0.0f )
        s->master_volume = 1.0f;

    printf( "[audio] init  master_volume=%.2f\n", s->master_volume );
    return true;
}

static bool
audio_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s = ( audio_state_t* )raw_state;
    printf( "[audio] reloaded  master_volume=%.2f\n", s->master_volume );
    return true;
}

static void
audio_exit( void* raw_state )
{
    audio_state_t* a = ( audio_state_t* )raw_state;
    for ( int i = 0; i < AUDIO_MAX_SOUNDS; ++i ) a->sounds[ i ].active = false;
    printf( "[audio] exit\n" );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
audio_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( audio_state_t ),
        .func_api_size = sizeof( audio_api_t ),
        .func_api      = &g_audio_api_struct,
        .deps          = { "core" },
        .dep_count     = 1,
        .init          = audio_init,
        .exit          = audio_exit,
        .reload        = audio_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( audio )

/*============================================================================================*/
