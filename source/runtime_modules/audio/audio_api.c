/*==============================================================================================

    audio_api.c

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
    ( void )get_api;
    s = ( audio_state_t* )raw_state;

    /* State is zeroed on first call; master_volume of 0 means uninitialised. */
    if ( s->master_volume == 0.0f )
        s->master_volume = 1.0f;

    printf( "[audio] init  master_volume=%.2f\n", s->master_volume );
    return true;
}

static void
audio_tick( void* raw_state, float dt )
{
    ( void )raw_state;
    ( void )dt;
    /* submit audio buffer to OS driver */
}

static void
audio_exit( void* raw_state )
{
    audio_state_t* a = ( audio_state_t* )raw_state;
    for ( int i = 0; i < AUDIO_MAX_SOUNDS; ++i ) a->sounds[ i ].active = false;
    printf( "[audio] exit\n" );
}

static void
audio_on_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    /* Re-anchor the local state pointer; no sibling APIs to re-cache. */
    s = ( audio_state_t* )raw_state;
    printf( "[audio] reloaded  master_volume=%.2f\n", s->master_volume );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
audio_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( audio_state_t ),
        .deps       = { "core" },
        .dep_count  = 1,
        .func_api   = &g_audio_api_struct, /* the globally-visible struct above */
        .init       = audio_init,
        .tick       = audio_tick,
        .exit       = audio_exit,
        .reload     = audio_on_reload,
    };
    return &api;
};

void*
audio_get_api( void )
{
    return ( void* )&g_audio_api_struct;
}

/* Emits undecorated DLL exports in dynamic builds; expands to nothing in static builds. */
MOD_DEFINE_EXPORTS( audio )

/*============================================================================================*/