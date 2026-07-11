/*==============================================================================================

    runtime_service/ahi/ahi_api.c -- AHI API struct wiring + module descriptor.

    Included LAST by ahi.c.  ahi_mixer.c has defined every static function in this
    translation unit; here they are assigned into the vtable and wrapped in the mod_desc_t
    lifecycle used by mod_static_load.

==============================================================================================*/

#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

const ahi_api_t g_ahi_api_struct =
{
    .play            = ahi_play,

    .voice_stop      = ahi_voice_stop,
    .voice_playing   = ahi_voice_playing,
    .voice_set_gain  = ahi_voice_set_gain,
    .voice_set_pan   = ahi_voice_set_pan,
    .voice_set_pitch = ahi_voice_set_pitch,

    .bus_set_gain    = ahi_bus_set_gain,
    .master_set_gain = ahi_master_set_gain,
    .stop_all        = ahi_stop_all,

    .sample_rate     = ahi_sample_rate,
    .voice_count     = ahi_voice_count,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
ahi_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );

    if ( !MOD_FETCH_CORE )    // dep "core": logging is up before the device starts
        return false;

    ahi_system_init();

    if ( s_ahi.device_rate )
        LOG_INFO( "device up: %u Hz, %u ch, %d voices", s_ahi.device_rate, AHI_CHANNELS, AHI_MAX_VOICES );
    else
        LOG_WARN( "no output device -- running silent (play() returns invalid)" );
    return true;
}

static bool
ahi_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    return MOD_FETCH_CORE;    // static service: no DLL swap, just re-cache siblings
}

static void
ahi_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    ahi_system_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
ahi_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,        /* mixer state lives in file-scope globals */
        .func_api_size = sizeof( ahi_api_t ),
        .dep_count     = 1,
        .deps          = { "core" },
        .func_api      = &g_ahi_api_struct,
        .init          = ahi_mod_init,
        .reload        = ahi_mod_reload,
        .exit          = ahi_mod_exit,
    };
    return &desc;
}

// clang-format on
/*============================================================================================*/
