/*==============================================================================================

    audio_api.c -- audio module wiring.
    Implements the audio_api_t vtable struct and the mod_desc_t lifecycle descriptor.

    Tier 2 of the audio stack: name -> bank lookup -> ahi() command.  Never touches the
    device or the audio thread, so a hot-reload mid-playback is safe -- live voices keep
    playing out of the ahi mixer while the DLL swaps.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers
==============================================================================================*/

MOD_USE_AHI;

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)

    The bank stores ahi_sound_t BY VALUE but the PCM it points at is caller-owned; both
    the pointers and this table survive a reload, so playback continues seamlessly.
==============================================================================================*/

typedef struct audio_state_s
{
    f32 master_volume;
    u32 sound_count;

    struct
    {
        char        name[ AUDIO_NAME_MAX ];
        ahi_sound_t snd;
    } bank[ AUDIO_MAX_SOUNDS ];

} audio_state_t;

static audio_state_t* s = NULL;

/*==============================================================================================
    Implementation
==============================================================================================*/

static const ahi_sound_t*
audio_find( const char* name )
{
    for ( u32 i = 0; i < s->sound_count; ++i )
        if ( strcmp( s->bank[ i ].name, name ) == 0 )
            return &s->bank[ i ].snd;
    return NULL;
}

static bool
audio_sound_register( const char* name, const ahi_sound_t* sound )
{
    if ( !name || !name[ 0 ] || strlen( name ) >= AUDIO_NAME_MAX || !sound )
        return false;

    /* Re-register replaces the PCM under the same name (hot-reload friendly). */
    for ( u32 i = 0; i < s->sound_count; ++i )
    {
        if ( strcmp( s->bank[ i ].name, name ) == 0 )
        {
            s->bank[ i ].snd = *sound;
            return true;
        }
    }

    if ( s->sound_count >= AUDIO_MAX_SOUNDS )
        return false;

    strcpy( s->bank[ s->sound_count ].name, name );
    s->bank[ s->sound_count ].snd = *sound;
    s->sound_count++;
    return true;
}

static audio_handle_t
audio_play_ex( const char* name, f32 volume, f32 pan, f32 pitch, bool loop )
{
    const ahi_sound_t* snd = audio_find( name );
    if ( !snd )
        return AUDIO_HANDLE_INVALID;

    ahi_params_t p = ahi_params_default();
    p.gain         = volume;
    p.pan          = pan;
    p.pitch        = pitch;
    p.loop         = loop;

    return ahi()->play( snd, &p ).id;
}

static audio_handle_t
audio_play( const char* name, f32 volume )
{
    return audio_play_ex( name, volume, 0.0f, 1.0f, false );
}

static void
audio_stop( audio_handle_t h )
{
    ahi_voice_t v = { h };
    ahi()->voice_stop( v );
}

static bool
audio_playing( audio_handle_t h )
{
    ahi_voice_t v = { h };
    return ahi()->voice_playing( v );
}

static void
audio_master_volume( f32 volume )
{
    s->master_volume = volume;
    ahi()->master_set_gain( volume );
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const audio_api_t g_audio_api_struct = {
    .sound_register = audio_sound_register,
    .play           = audio_play,
    .play_ex        = audio_play_ex,
    .stop           = audio_stop,
    .playing        = audio_playing,
    .master_volume  = audio_master_volume,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
audio_init( void* raw_state, get_api_fn get_api )
{
    s = ( audio_state_t* )raw_state;

    if ( !MOD_FETCH_AHI )
        return false;

    /* state is zeroed on first load; 0 means never set */
    if ( s->master_volume == 0.0f )
        s->master_volume = 1.0f;

    return true;
}

static bool
audio_reload( void* raw_state, get_api_fn get_api )
{
    s = ( audio_state_t* )raw_state;
    return MOD_FETCH_AHI;    // bank + live voices carry over; just re-cache the service
}

static void
audio_exit( void* raw_state )
{
    UNUSED( raw_state );
    ahi()->stop_all();    // every live voice points at PCM we can no longer vouch for
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
        .deps          = { "ahi" },
        .dep_count     = 1,
        .init          = audio_init,
        .exit          = audio_exit,
        .reload        = audio_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( audio )

/*============================================================================================*/
