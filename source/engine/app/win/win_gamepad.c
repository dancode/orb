/*==============================================================================================

    engine/app/win/win_gamepad.c — XInput gamepad backend.

    Polling model
    -------------
    XInput is a polled API: win_gamepad_poll() runs at the top of every pump_events(), after
    input_snapshot(), so pad edges land in the same frame window as keyboard edges.  Button
    state is folded into the unified source arrays (g_input.current_keys / pressed_keys at
    the APP_SRC_PAD_* codes) and each edge posts an APP_EV_KEY_DOWN / APP_EV_KEY_UP into the
    ring -- a pad button is indistinguishable from a keyboard key downstream (snapshot
    queries, event drain, bind routing).

    Multiple pads OR into the shared digital state: an edge fires when the combined mask
    changes.  Per-pad addressing exists only for the analog queries (pad_axis) and rumble;
    per-pad buttons arrive with player assignment in the input service.

    Empty-slot cost
    ---------------
    XInputGetState on a disconnected slot triggers device enumeration internally and is very
    slow.  Connected slots poll every frame; disconnected slots re-scan on a timer only.

    Axes are raw normalized hardware values -- no deadzone, no response curve.  Those are
    policy and live in the input service where they are cvar-tunable.

==============================================================================================*/

#include <xinput.h>
#pragma comment( lib, "xinput9_1_0.lib" ) /* ships with Windows -- no redistributable */

#define WIN_PAD_RESCAN_MS 1500 /* disconnected-slot re-scan period          */
#define WIN_PAD_TRIG_ON   64   /* trigger digital press threshold (0..255)  */
#define WIN_PAD_TRIG_OFF  48   /* release threshold -- hysteresis gap       */

typedef struct win_pad_s
{
    bool         connected;
    bool         trig_l; /* digital trigger latch (hysteresis) */
    bool         trig_r;
    XINPUT_STATE state;

} win_pad_t;

static struct
{
    win_pad_t pads[ APP_PAD_MAX ];
    i64       next_rescan_ms;

} g_pad;

/* XInput button bit -> unified source code, walked once per poll. */
/* clang-format off */
static const struct { u16 xi_bit; u16 src; } k_pad_button_map[] =
{
    { XINPUT_GAMEPAD_A,              APP_SRC_PAD_A          },
    { XINPUT_GAMEPAD_B,              APP_SRC_PAD_B          },
    { XINPUT_GAMEPAD_X,              APP_SRC_PAD_X          },
    { XINPUT_GAMEPAD_Y,              APP_SRC_PAD_Y          },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  APP_SRC_PAD_LB         },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, APP_SRC_PAD_RB         },
    { XINPUT_GAMEPAD_BACK,           APP_SRC_PAD_BACK       },
    { XINPUT_GAMEPAD_START,          APP_SRC_PAD_START      },
    { XINPUT_GAMEPAD_LEFT_THUMB,     APP_SRC_PAD_LS         },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    APP_SRC_PAD_RS         },
    { XINPUT_GAMEPAD_DPAD_UP,        APP_SRC_PAD_DPAD_UP    },
    { XINPUT_GAMEPAD_DPAD_DOWN,      APP_SRC_PAD_DPAD_DOWN  },
    { XINPUT_GAMEPAD_DPAD_LEFT,      APP_SRC_PAD_DPAD_LEFT  },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     APP_SRC_PAD_DPAD_RIGHT },
};
/* clang-format on */

/*============================================================================================*/
/* Digital edge: mirror the keyboard path exactly -- update the unified snapshot arrays and
   post a KEY event carrying the pad source code.  win_id is the main window: a pad is not
   tied to a window, and app-level consumers key routing off the main surface. */

static void
win_pad_post_edge( u16 src, bool down )
{
    g_input.current_keys[ src ] = down;
    if ( down )
        g_input.pressed_keys[ src ] = true;

    app_event_t ev    = win_make_event( down ? APP_EV_KEY_DOWN : APP_EV_KEY_UP, g_pool.main_id );
    ev.data.key.key   = ( i32 )src;
    ev.data.key.press = down ? 255 : 0;
    win_post_event( &ev );
}

/*============================================================================================*/
/* Per-frame poll: connected slots every frame, disconnected slots on the re-scan timer.
   Edge detection diffs the combined (all pads OR) state against g_input.current_keys, so
   the unified arrays are the single source of truth for previous state. */

static void
win_gamepad_poll( void )
{
    const i64 now_ms = ( i64 )GetTickCount64();
    bool      rescan = now_ms >= g_pad.next_rescan_ms;
    if ( rescan )
        g_pad.next_rescan_ms = now_ms + WIN_PAD_RESCAN_MS;

    u32  combined = 0;
    bool any_lt   = false;
    bool any_rt   = false;

    for ( u32 i = 0; i < APP_PAD_MAX; ++i )
    {
        win_pad_t* p = &g_pad.pads[ i ];

        if ( !p->connected && !rescan )
            continue;

        XINPUT_STATE st;
        if ( XInputGetState( i, &st ) == ERROR_SUCCESS )
        {
            if ( !p->connected )
            {
                p->connected = true;
                app_log( ORB_LOG_INFO, "gamepad %u connected", i );
            }
            p->state = st;
        }
        else if ( p->connected )
        {
            p->connected = false;
            p->trig_l    = false;
            p->trig_r    = false;
            memset( &p->state, 0, sizeof( p->state ) );
            app_log( ORB_LOG_INFO, "gamepad %u disconnected", i );
        }

        if ( p->connected )
        {
            combined |= p->state.Gamepad.wButtons;

            /* Trigger digital latch with hysteresis: a trigger resting on the threshold
               must not machine-gun press/release pairs. */
            const u8 lt = p->state.Gamepad.bLeftTrigger;
            const u8 rt = p->state.Gamepad.bRightTrigger;
            p->trig_l   = p->trig_l ? ( lt > WIN_PAD_TRIG_OFF ) : ( lt >= WIN_PAD_TRIG_ON );
            p->trig_r   = p->trig_r ? ( rt > WIN_PAD_TRIG_OFF ) : ( rt >= WIN_PAD_TRIG_ON );
            any_lt |= p->trig_l;
            any_rt |= p->trig_r;
        }
    }

    for ( u32 i = 0; i < ARRAY_COUNT( k_pad_button_map ); ++i )
    {
        const bool down = ( combined & k_pad_button_map[ i ].xi_bit ) != 0;
        if ( down != g_input.current_keys[ k_pad_button_map[ i ].src ] )
            win_pad_post_edge( k_pad_button_map[ i ].src, down );
    }

    if ( any_lt != g_input.current_keys[ APP_SRC_PAD_LTRIGGER ] )
        win_pad_post_edge( APP_SRC_PAD_LTRIGGER, any_lt );
    if ( any_rt != g_input.current_keys[ APP_SRC_PAD_RTRIGGER ] )
        win_pad_post_edge( APP_SRC_PAD_RTRIGGER, any_rt );
}

/*==============================================================================================
    API implementations
==============================================================================================*/

static bool
app_pad_connected( i32 pad )
{
    if ( pad < 0 || pad >= APP_PAD_MAX )
        return false;
    return g_pad.pads[ pad ].connected;
}

/* Asymmetric stick range: SHORT spans -32768..32767, so divide by the matching magnitude
   to hit exactly -1..1 at both rails. */
static f32
pad_norm_stick( SHORT v )
{
    return ( v < 0 ) ? ( f32 )v / 32768.0f : ( f32 )v / 32767.0f;
}

static f32
app_pad_axis( i32 pad, app_pad_axis_t axis )
{
    if ( pad < 0 || pad >= APP_PAD_MAX || !g_pad.pads[ pad ].connected )
        return 0.0f;

    const XINPUT_GAMEPAD* g = &g_pad.pads[ pad ].state.Gamepad;

    switch ( axis )
    {
        case APP_PAD_AXIS_LX: return pad_norm_stick( g->sThumbLX );
        case APP_PAD_AXIS_LY: return pad_norm_stick( g->sThumbLY );
        case APP_PAD_AXIS_RX: return pad_norm_stick( g->sThumbRX );
        case APP_PAD_AXIS_RY: return pad_norm_stick( g->sThumbRY );
        case APP_PAD_AXIS_LT: return ( f32 )g->bLeftTrigger / 255.0f;
        case APP_PAD_AXIS_RT: return ( f32 )g->bRightTrigger / 255.0f;
        default:              return 0.0f;
    }
}

static void
app_pad_rumble( i32 pad, f32 lo, f32 hi )
{
    if ( pad < 0 || pad >= APP_PAD_MAX || !g_pad.pads[ pad ].connected )
        return;

    if ( lo < 0.0f ) lo = 0.0f;
    if ( lo > 1.0f ) lo = 1.0f;
    if ( hi < 0.0f ) hi = 0.0f;
    if ( hi > 1.0f ) hi = 1.0f;

    XINPUT_VIBRATION vib = {
        .wLeftMotorSpeed  = ( WORD )( lo * 65535.0f ),
        .wRightMotorSpeed = ( WORD )( hi * 65535.0f ),
    };
    XInputSetState( ( DWORD )pad, &vib );
}

/*============================================================================================*/
