/*==============================================================================================

    runtime_service/ahi/ahi_mixer.c -- voice pool, command ring, mix loop.

    Two threads touch this file:

        MAIN thread   -- the ahi_* API functions: claim a voice slot, push commands.
        AUDIO thread  -- ahi_mix(): drain commands, advance voices, sum into the buffer.

    Ownership rules that make it lock-free:
        - slot state FREE -> PENDING is the only main-thread transition (CAS claim);
          PENDING -> PLAYING and PLAYING -> FREE happen only on the audio thread.
        - voice fields (sound, gain, pan, ...) are written by the main thread only while
          the slot is PENDING and unseen; after the PLAY command publishes, all mutation
          goes through the command ring.
        - the ring is single-producer (main) / single-consumer (audio): monotonically
          increasing head/tail indices, power-of-two wrap.

==============================================================================================*/

/*==============================================================================================
    Internal state
==============================================================================================*/

#define AHI_CMD_RING 256    /* must be a power of two */

enum
{
    AHI_SLOT_FREE = 0,
    AHI_SLOT_PENDING,    // claimed + being filled by the main thread
    AHI_SLOT_PLAYING,
};

enum
{
    AHI_CMD_PLAY = 0,
    AHI_CMD_STOP,
    AHI_CMD_GAIN,
    AHI_CMD_PAN,
    AHI_CMD_PITCH,
    AHI_CMD_BUS_GAIN,     // slot = bus index
    AHI_CMD_MASTER_GAIN,
    AHI_CMD_STOP_ALL,
};

typedef struct ahi_cmd_s
{
    u16 op;
    u16 slot;
    i32 gen;      // handle generation for voice commands; stale = ignored
    f32 value;

} ahi_cmd_t;

typedef struct ahi_voice_slot_s
{
    volatile i32 state;    // AHI_SLOT_*
    volatile i32 gen;      // bumped when the voice frees; stale handles miss

    ahi_sound_t  snd;      // audio-thread-owned once PLAYING
    f32          gain;
    f32          pan;
    f32          pitch;
    u32          bus;
    b32          loop;
    f64          cursor;   // fractional frame position into snd

} ahi_voice_slot_t;

typedef struct ahi_state_s
{
    ahi_voice_slot_t voices[ AHI_MAX_VOICES ];

    ahi_cmd_t        cmds[ AHI_CMD_RING ];
    volatile i32     cmd_head;              // main thread writes
    volatile i32     cmd_tail;              // audio thread writes

    f32              bus_gain[ AHI_BUS_COUNT ];    // audio-thread-owned after init
    f32              master;                       // audio-thread-owned after init

    u32              device_rate;           // 0 = no device, mixer is silent
    volatile i32     live_voices;           // stats: pending + playing, audio thread writes

} ahi_state_t;

static ahi_state_t s_ahi;

/*==============================================================================================
    Handles
==============================================================================================*/

#define AHI_GEN_MASK 0x00FFFFFF

static ahi_voice_t
ahi_handle_make( i32 slot, i32 gen )
{
    ahi_voice_t v = { ( ( u32 )( gen & AHI_GEN_MASK ) << 8 ) | ( u32 )( slot + 1 ) };
    return v;
}

/* Returns the slot index, or -1 for null/garbage ids.  Generation is NOT checked here. */
static i32
ahi_handle_decode( ahi_voice_t v, i32* out_gen )
{
    i32 slot = ( i32 )( v.id & 0xFF ) - 1;
    if ( slot < 0 || slot >= AHI_MAX_VOICES )
        return -1;
    *out_gen = ( i32 )( v.id >> 8 );
    return slot;
}

/* True when the handle still names the voice living in its slot. */
static bool
ahi_handle_live( ahi_voice_t v, i32* out_slot )
{
    i32 gen;
    i32 slot = ahi_handle_decode( v, &gen );
    if ( slot < 0 )
        return false;
    if ( ( sys_atomic_read( &s_ahi.voices[ slot ].gen ) & AHI_GEN_MASK ) != gen )
        return false;
    *out_slot = slot;
    return true;
}

/*==============================================================================================
    Command ring  (main thread produces, audio thread consumes)
==============================================================================================*/

static bool
ahi_cmd_push( u16 op, u16 slot, i32 gen, f32 value )
{
    i32 head = sys_atomic_read( &s_ahi.cmd_head );
    i32 tail = sys_atomic_read( &s_ahi.cmd_tail );
    if ( head - tail >= AHI_CMD_RING )
        return false;    // ring full: drop; audio keeps running, caller may warn

    ahi_cmd_t* c = &s_ahi.cmds[ head & ( AHI_CMD_RING - 1 ) ];
    c->op    = op;
    c->slot  = slot;
    c->gen   = gen;
    c->value = value;

    sys_atomic_write( &s_ahi.cmd_head, head + 1 );    // publish (full fence)
    return true;
}

/* AUDIO THREAD.  Voice frees bump the generation first so main-thread handle checks
   never see a freed slot under a live generation. */
static void
ahi_voice_free( ahi_voice_slot_t* v )
{
    sys_atomic_increment( &v->gen );
    sys_atomic_write( &v->state, AHI_SLOT_FREE );
}

/* AUDIO THREAD.  Applies every command queued since the last mix block. */
static void
ahi_drain_commands( void )
{
    i32 head = sys_atomic_read( &s_ahi.cmd_head );
    i32 tail = s_ahi.cmd_tail;

    for ( ; tail != head; ++tail )
    {
        const ahi_cmd_t*  c = &s_ahi.cmds[ tail & ( AHI_CMD_RING - 1 ) ];
        ahi_voice_slot_t* v = &s_ahi.voices[ c->slot ];

        switch ( c->op )
        {
            case AHI_CMD_PLAY:
                if ( v->state == AHI_SLOT_PENDING )
                    sys_atomic_write( &v->state, AHI_SLOT_PLAYING );
                break;

            case AHI_CMD_STOP:
                if ( v->state == AHI_SLOT_PLAYING && ( v->gen & AHI_GEN_MASK ) == c->gen )
                    ahi_voice_free( v );
                break;

            case AHI_CMD_GAIN:
                if ( v->state == AHI_SLOT_PLAYING && ( v->gen & AHI_GEN_MASK ) == c->gen )
                    v->gain = c->value;
                break;

            case AHI_CMD_PAN:
                if ( v->state == AHI_SLOT_PLAYING && ( v->gen & AHI_GEN_MASK ) == c->gen )
                    v->pan = c->value;
                break;

            case AHI_CMD_PITCH:
                if ( v->state == AHI_SLOT_PLAYING && ( v->gen & AHI_GEN_MASK ) == c->gen )
                    v->pitch = c->value;
                break;

            case AHI_CMD_BUS_GAIN:
                if ( c->slot < AHI_BUS_COUNT )
                    s_ahi.bus_gain[ c->slot ] = c->value;
                break;

            case AHI_CMD_MASTER_GAIN:
                s_ahi.master = c->value;
                break;

            case AHI_CMD_STOP_ALL:
                for ( i32 i = 0; i < AHI_MAX_VOICES; ++i )
                    if ( s_ahi.voices[ i ].state == AHI_SLOT_PLAYING )
                        ahi_voice_free( &s_ahi.voices[ i ] );
                break;
        }
    }

    sys_atomic_write( &s_ahi.cmd_tail, tail );
}

/*==============================================================================================
    Mix loop  (AUDIO THREAD -- the backend calls this whenever the device wants frames)
==============================================================================================*/

static void
ahi_mix( f32* out, u32 frames )
{
    ahi_drain_commands();

    memset( out, 0, ( usize )frames * AHI_CHANNELS * sizeof( f32 ) );

    i32 live = 0;

    for ( i32 i = 0; i < AHI_MAX_VOICES; ++i )
    {
        ahi_voice_slot_t* v = &s_ahi.voices[ i ];
        if ( v->state == AHI_SLOT_PENDING )
        {
            live++;    // claimed but PLAY not drained yet; audible next block
            continue;
        }
        if ( v->state != AHI_SLOT_PLAYING )
            continue;

        /* Per-block constants: params only mutate at drain time, above. */
        const f32* src   = v->snd.frames;
        const u32  count = v->snd.frame_count;
        const u32  ch    = v->snd.channels;
        const f32  amp   = v->gain * s_ahi.bus_gain[ v->bus ] * s_ahi.master;
        const f32  lg    = amp * f32_sqrt( 0.5f * ( 1.0f - v->pan ) );
        const f32  rg    = amp * f32_sqrt( 0.5f * ( 1.0f + v->pan ) );
        const f64  step  = ( f64 )v->snd.sample_rate * v->pitch / ( f64 )s_ahi.device_rate;

        f64  cur  = v->cursor;
        bool done = false;

        for ( u32 f = 0; f < frames; ++f )
        {
            if ( cur >= ( f64 )count )
            {
                if ( !v->loop )
                {
                    done = true;
                    break;
                }
                cur -= ( f64 )count;
            }

            /* Linear resample; the last frame lerps toward the loop start or holds. */
            u32 i0 = ( u32 )cur;
            u32 i1 = i0 + 1;
            if ( i1 >= count )
                i1 = v->loop ? 0 : i0;
            f32 t = ( f32 )( cur - ( f64 )i0 );

            f32 sl, sr;
            if ( ch == 1 )
            {
                f32 a = src[ i0 ];
                sl = sr = a + ( src[ i1 ] - a ) * t;
            }
            else
            {
                f32 al = src[ i0 * 2 ];
                f32 ar = src[ i0 * 2 + 1 ];
                sl     = al + ( src[ i1 * 2 ] - al ) * t;
                sr     = ar + ( src[ i1 * 2 + 1 ] - ar ) * t;
            }

            out[ f * 2 ]     += sl * lg;
            out[ f * 2 + 1 ] += sr * rg;
            cur += step;
        }

        v->cursor = cur;
        if ( done )
            ahi_voice_free( v );
        else
            live++;
    }

    /* Hard clip: keeps a hot mix from wrapping in the DAC; proper limiting can come later. */
    for ( u32 n = 0; n < frames * AHI_CHANNELS; ++n )
    {
        f32 x    = out[ n ];
        out[ n ] = ( x < -1.0f ) ? -1.0f : ( x > 1.0f ) ? 1.0f : x;
    }

    sys_atomic_write( &s_ahi.live_voices, live );
}

/*==============================================================================================
    API implementation  (MAIN THREAD)
==============================================================================================*/

static ahi_voice_t
ahi_play( const ahi_sound_t* sound, const ahi_params_t* params )
{
    ahi_voice_t invalid = { AHI_VOICE_INVALID };

    if ( !s_ahi.device_rate || !sound || !sound->frames || !sound->frame_count )
        return invalid;
    if ( sound->channels != 1 && sound->channels != 2 )
        return invalid;

    /* Claim a free slot: the CAS is the only main-thread state transition. */
    i32 slot = -1;
    for ( i32 i = 0; i < AHI_MAX_VOICES; ++i )
    {
        if ( sys_atomic_compare_exchange( &s_ahi.voices[ i ].state, AHI_SLOT_PENDING,
                                          AHI_SLOT_FREE ) == AHI_SLOT_FREE )
        {
            slot = i;
            break;
        }
    }
    if ( slot < 0 )
        return invalid;

    ahi_voice_slot_t* v = &s_ahi.voices[ slot ];
    ahi_params_t      p = params ? *params : ahi_params_default();

    v->snd    = *sound;
    v->gain   = p.gain;
    v->pan    = p.pan;
    v->pitch  = ( p.pitch > 0.0f ) ? p.pitch : 1.0f;
    v->bus    = ( p.bus < AHI_BUS_COUNT ) ? p.bus : AHI_BUS_SFX;
    v->loop   = p.loop;
    v->cursor = 0.0;

    i32 gen = sys_atomic_read( &v->gen );

    if ( !ahi_cmd_push( AHI_CMD_PLAY, ( u16 )slot, gen, 0.0f ) )
    {
        sys_atomic_write( &v->state, AHI_SLOT_FREE );    // ring full: unclaim
        return invalid;
    }
    return ahi_handle_make( slot, gen );
}

static void
ahi_voice_stop( ahi_voice_t voice )
{
    i32 slot;
    if ( ahi_handle_live( voice, &slot ) )
        ahi_cmd_push( AHI_CMD_STOP, ( u16 )slot, ( i32 )( voice.id >> 8 ), 0.0f );
}

static bool
ahi_voice_playing( ahi_voice_t voice )
{
    i32 slot;
    if ( !ahi_handle_live( voice, &slot ) )
        return false;
    return sys_atomic_read( &s_ahi.voices[ slot ].state ) != AHI_SLOT_FREE;
}

static void
ahi_voice_set_gain( ahi_voice_t voice, f32 gain )
{
    i32 slot;
    if ( ahi_handle_live( voice, &slot ) )
        ahi_cmd_push( AHI_CMD_GAIN, ( u16 )slot, ( i32 )( voice.id >> 8 ), gain );
}

static void
ahi_voice_set_pan( ahi_voice_t voice, f32 pan )
{
    i32 slot;
    if ( ahi_handle_live( voice, &slot ) )
        ahi_cmd_push( AHI_CMD_PAN, ( u16 )slot, ( i32 )( voice.id >> 8 ),
                      ( pan < -1.0f ) ? -1.0f : ( pan > 1.0f ) ? 1.0f : pan );
}

static void
ahi_voice_set_pitch( ahi_voice_t voice, f32 pitch )
{
    i32 slot;
    if ( ahi_handle_live( voice, &slot ) && pitch > 0.0f )
        ahi_cmd_push( AHI_CMD_PITCH, ( u16 )slot, ( i32 )( voice.id >> 8 ), pitch );
}

static void
ahi_bus_set_gain( u32 bus, f32 gain )
{
    if ( bus < AHI_BUS_COUNT )
        ahi_cmd_push( AHI_CMD_BUS_GAIN, ( u16 )bus, 0, gain );
}

static void
ahi_master_set_gain( f32 gain )
{
    ahi_cmd_push( AHI_CMD_MASTER_GAIN, 0, 0, gain );
}

static void
ahi_stop_all( void )
{
    ahi_cmd_push( AHI_CMD_STOP_ALL, 0, 0, 0.0f );
}

static u32
ahi_sample_rate( void )
{
    return s_ahi.device_rate;
}

static u32
ahi_voice_count( void )
{
    return ( u32 )sys_atomic_read( &s_ahi.live_voices );
}

/*==============================================================================================
    Lifecycle  (called from ahi_api.c's mod hooks)
==============================================================================================*/

static void
ahi_system_init( void )
{
    memset( &s_ahi, 0, sizeof( s_ahi ) );

    s_ahi.master = 1.0f;
    for ( i32 i = 0; i < AHI_BUS_COUNT; ++i ) s_ahi.bus_gain[ i ] = 1.0f;

    if ( ahi_backend_start( ahi_mix ) )
        s_ahi.device_rate = AHI_SAMPLE_RATE;
}

static void
ahi_system_exit( void )
{
    ahi_backend_stop();
    s_ahi.device_rate = 0;
}

/*============================================================================================*/
