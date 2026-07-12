/*==============================================================================================

    sandbox/runtime/sb_audio/sb_audio.c -- End-to-end proof of the audio stack.

    The two tiers under test:

        ahi   (runtime_service/ahi)    -- Tier 1 STATIC service: WASAPI device + audio
                                          thread + lock-free mixer.  Called directly here
                                          for voice param sweeps.
        audio (runtime_modules/audio)  -- Tier 2 hot-reload module: name -> bank -> ahi
                                          commands.  The layer game code talks to.

    Runs scripted (no window, no input): generates a few PCM sounds, registers them in
    the module's bank, plays them through both tiers, sweeps pan/pitch on a looping
    voice, and verifies handle lifetime (finish, stop, stale, stop_all).  You should
    HEAR: a beep, a noise burst, then a two-second hum gliding left-to-right and up in
    pitch.  Exit code is the number of failed checks.

    With no output device the ahi service loads in silent mode; playback checks are
    skipped and the run still passes.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "base/base.h"    /* f32_sin + rng for PCM generation */
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/ref/ref_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/ahi/ahi_host.h"
#include "runtime_modules/audio/audio_api.h"

MOD_USE_AUDIO;

/*==============================================================================================
    Checks
==============================================================================================*/

static int s_pass;
static int s_fail;

static void
check( bool ok, const char* what )
{
    if ( ok )
        s_pass++;
    else
        s_fail++;
    printf( "  %s  %s\n", ok ? "PASS" : "FAIL", what );
}

/* Poll a tier-2 handle until it stops playing; false = still alive at the deadline. */
static bool
wait_stopped( audio_handle_t h, i32 timeout_ms )
{
    for ( i32 t = 0; t < timeout_ms; t += 10 )
    {
        if ( !audio()->playing( h ) )
            return true;
        sys_sleep_milliseconds( 10 );
    }
    return false;
}

/*==============================================================================================
    Generated PCM  (mono f32 at the device rate; static so hot-reload could survive us)
==============================================================================================*/

#define GEN_RATE 48000

static f32 s_beep[ GEN_RATE / 4 ];      /* 0.25 s  440 Hz sine, faded tail   */
static f32 s_noise[ GEN_RATE / 5 ];     /* 0.20 s  white noise burst         */
static f32 s_hum[ GEN_RATE / 2 ];       /* 0.50 s  220 Hz sine, loops clean  */

static void
generate_sounds( void )
{
    for ( u32 i = 0; i < ARRAY_COUNT( s_beep ); ++i )
    {
        f32 t         = ( f32 )i / GEN_RATE;
        f32 fade      = 1.0f - ( f32 )i / ARRAY_COUNT( s_beep );
        s_beep[ i ]   = f32_sin( MATH_TAU * 440.0f * t ) * fade;
    }

    rng_t rng;
    rng_seed( &rng, 0xB00F );
    for ( u32 i = 0; i < ARRAY_COUNT( s_noise ); ++i )
    {
        f32 fade     = 1.0f - ( f32 )i / ARRAY_COUNT( s_noise );
        s_noise[ i ] = ( rng_f32( &rng ) * 2.0f - 1.0f ) * 0.5f * fade;
    }

    /* 220 Hz over 0.5 s = 110 whole cycles, so the loop seam is silent. */
    for ( u32 i = 0; i < ARRAY_COUNT( s_hum ); ++i )
        s_hum[ i ] = f32_sin( MATH_TAU * 220.0f * ( f32 )i / GEN_RATE ) * 0.6f;
}

static ahi_sound_t
mono_sound( const f32* frames, u32 frame_count )
{
    ahi_sound_t s = { frames, frame_count, 1, GEN_RATE };
    return s;
}

/*============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    ref_wire_mod_callbacks();
    core_wire_mod_callbacks();

    mod_static( sys );
    mod_static( ref );
    mod_static( core );
    mod_static( ahi );

    if ( !mod_load( audio ) )
    {
        fprintf( stderr, "load audio: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    if ( !mod_init_all() )
    {
        fprintf( stderr, "mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    MOD_HOST_FETCH_API( audio );

    printf( "\n=== sb_audio: ahi service + audio module proof ===\n\n" );

    generate_sounds();

    /* ---- bank registration (tier 2) ------------------------------------------------- */

    ahi_sound_t beep  = mono_sound( s_beep, ARRAY_COUNT( s_beep ) );
    ahi_sound_t noise = mono_sound( s_noise, ARRAY_COUNT( s_noise ) );
    ahi_sound_t hum   = mono_sound( s_hum, ARRAY_COUNT( s_hum ) );

    check( audio()->sound_register( "beep", &beep ), "register beep" );
    check( audio()->sound_register( "noise", &noise ), "register noise" );
    check( audio()->sound_register( "hum", &hum ), "register hum" );
    check( audio()->sound_register( "beep", &beep ), "re-register replaces, not fails" );
    check( audio()->play( "nope", 1.0f ) == AUDIO_HANDLE_INVALID, "unknown name rejected" );

    if ( ahi()->sample_rate() == 0 )
    {
        printf( "\nno output device -- skipping playback checks\n" );
        mod_system_exit();
        printf( "\nsb_audio: %d passed, %d failed\n", s_pass, s_fail );
        return s_fail;
    }

    printf( "\ndevice: %u Hz, %d voice slots\n\n", ahi()->sample_rate(), AHI_MAX_VOICES );
    audio()->master_volume( 0.5f );

    /* ---- one-shot lifetime (tier 2) -------------------------------------------------- */

    audio_handle_t h = audio()->play( "beep", 1.0f );
    check( h != AUDIO_HANDLE_INVALID, "play beep" );
    sys_sleep_milliseconds( 60 );
    check( audio()->playing( h ), "beep reports playing" );
    check( ahi()->voice_count() >= 1, "voice_count sees it" );
    check( wait_stopped( h, 1000 ), "beep finishes on its own" );
    check( !audio()->playing( h ), "finished handle reads stopped (stale-safe)" );

    audio_handle_t n = audio()->play( "noise", 0.8f );
    check( n != AUDIO_HANDLE_INVALID, "play noise" );
    check( wait_stopped( n, 1000 ), "noise finishes" );

    /* ---- looping voice + tier-1 param sweeps ----------------------------------------- */

    audio_handle_t loop = audio()->play_ex( "hum", 0.8f, -1.0f, 1.0f, true );
    check( loop != AUDIO_HANDLE_INVALID, "play looping hum" );

    ahi_voice_t voice = { loop };    /* tier-2 handle IS the tier-1 voice id */

    printf( "\n  sweeping pan left->right and pitch 1.0->1.5 over 2 s ...\n" );
    for ( i32 step = 0; step <= 100; ++step )
    {
        f32 t = ( f32 )step / 100.0f;
        ahi()->voice_set_pan( voice, -1.0f + 2.0f * t );
        ahi()->voice_set_pitch( voice, 1.0f + 0.5f * t );
        sys_sleep_milliseconds( 20 );
    }

    check( audio()->playing( loop ), "loop still playing after 2 s (loop works)" );
    audio()->stop( loop );
    sys_sleep_milliseconds( 100 );    /* let the mixer drain the stop command */
    check( !audio()->playing( loop ), "stopped loop reads stopped" );

    /* ---- stop_all ------------------------------------------------------------------- */

    audio_handle_t a = audio()->play_ex( "hum", 0.3f, -0.5f, 1.0f, true );
    audio_handle_t b = audio()->play_ex( "hum", 0.3f, 0.5f, 1.3f, true );
    check( a && b, "two concurrent loops start" );
    sys_sleep_milliseconds( 200 );

    ahi()->stop_all();
    sys_sleep_milliseconds( 100 );
    check( !audio()->playing( a ) && !audio()->playing( b ), "stop_all silences both" );
    check( ahi()->voice_count() == 0, "no live voices after stop_all" );

    mod_system_exit();

    printf( "\nsb_audio: %d passed, %d failed\n", s_pass, s_fail );
    return s_fail;
}

/*============================================================================================*/
