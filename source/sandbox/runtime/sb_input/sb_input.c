/*==============================================================================================

    sandbox/runtime/sb_input/sb_input.c -- End-to-end proof of the input action service (Phase 3).

    The full chain under test, in one window:

        bind w +forward          (cmd_bind, core)
        W key edge               (app event drain -> cmd_bind_event)
        "+forward 17" queued     (bind transport)
        cmd_pump()               (dispatches to the input service's +forward handler)
        input()->frame( dt )     (latches the edge into the frame state block)
        input()->down/pressed    (what a game reads)

    What to try:
        W/S/A/D, SPACE      -- held movement actions; UP also drives forward (overlapping
                               holds: press W and UP together, release one, still down)
        MOUSE1 / PAD_A      -- +attack / +jump from non-keyboard sources, same path
        F                   -- plain (non +/-) bind still works: echoes to the console
        TAB                 -- toggles a UI context push: all gameplay actions force-release
                               (hold W, hit TAB -> one clean released edge, no stuck key)
        ESC                 -- quit

    Phase 4 axis binds (the analog half -- table-driven, no command strings per frame):
        W/S/A/D             -- ALSO drive the "move" AXIS2 action as a digital composite
                               (bindaxis w move 0 1 etc.); [move] prints on change, W+D
                               gives a diagonal, TAB gates it to (0,0) like the buttons
        LEFT STICK          -- drives "move" too: radial deadzone + curve, sums with WASD,
                               clamps at -1..1 (holding W plus full stick never exceeds 1)
        MOUSE MOVEMENT      -- drives the "look" AXIS2 action from the raw delta channel
                               ([look] prints an accumulated total, +y up, in_mouse_sens)
        RIGHT STICK         -- also drives "look" (scaled up -- rate-style input)
        C                   -- writeconfig test_input.cfg: proves cvars + binds + bindaxis
                               lines all land in one file (check it next to the exe)

    Correct output: PRESS/RELEASE lines with per-frame counts, a state line whenever any
    down-state changes, [move] lines tracking the composite vector, and after TAB a forced
    release while the key is still held.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/ref/ref_host.h"
#include "engine/core/core_host.h"
#include "engine/app/app_host.h"
#include "runtime_service/input/input_host.h"

/* Caller-defined context bits -- the service only tests masks. */
#define CTX_GAME ( 1u << 0 )
#define CTX_UI   ( 1u << 1 )

typedef struct test_action_s
{
    const char*    name;
    input_action_t id;

} test_action_t;

static test_action_t s_acts[] = {
    { "forward",   INPUT_ACTION_INVALID },
    { "back",      INPUT_ACTION_INVALID },
    { "moveleft",  INPUT_ACTION_INVALID },
    { "moveright", INPUT_ACTION_INVALID },
    { "jump",      INPUT_ACTION_INVALID },
    { "attack",    INPUT_ACTION_INVALID },
};

static input_action_t s_move = INPUT_ACTION_INVALID;    // AXIS2: WASD composite + left stick
static input_action_t s_look = INPUT_ACTION_INVALID;    // AXIS2: mouse raw delta + right stick

/*============================================================================================*/
/* One line of current down-states, printed when anything changed. */

static void
print_state_line( void )
{
    printf( "[state]" );
    for ( u32 i = 0; i < ARRAY_COUNT( s_acts ); ++i )
        printf( " %s=%d", s_acts[ i ].name, input()->down( s_acts[ i ].id ) ? 1 : 0 );
    printf( "\n" );
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
    mod_static( app );
    mod_static( core );
    mod_static( input );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    win_id_t win = app()->window_open( "sb_input", 0, 0, 800, 500, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID )
    {
        fprintf( stderr, "window_open failed\n" );
        mod_system_exit();
        return 1;
    }

    /* Bind names for the whole unified source space (keyboard + mouse + pad). */
    cmd_bind_wire_names( app_key_names(), APP_SRC_COUNT );

    /* Register actions -- BUTTONs auto-register the +name/-name console commands. */
    for ( u32 i = 0; i < ARRAY_COUNT( s_acts ); ++i )
        s_acts[ i ].id = input()->action_register( s_acts[ i ].name, INPUT_ACTION_BUTTON, CTX_GAME );

    s_move = input()->action_register( "move", INPUT_ACTION_AXIS2, CTX_GAME );
    s_look = input()->action_register( "look", INPUT_ACTION_AXIS2, CTX_GAME );

    /* Binds through the normal console path -- exactly what a config file would run. */
    cmd_execute_string( "bind w +forward" );
    cmd_execute_string( "bind up +forward" );    // second source on the same action
    cmd_execute_string( "bind s +back" );
    cmd_execute_string( "bind a +moveleft" );
    cmd_execute_string( "bind d +moveright" );
    cmd_execute_string( "bind space +jump" );
    cmd_execute_string( "bind mouse1 +attack" );
    cmd_execute_string( "bind pad_a +jump" );
    cmd_execute_string( "bind f \"echo plain bind fired\"" );

    /* Axis binds: WASD digital composite + left stick into "move" (they sum and clamp);
       mouse raw delta + scaled right stick into "look". */
    cmd_execute_string( "bindaxis w move 0 1" );
    cmd_execute_string( "bindaxis s move 0 -1" );
    cmd_execute_string( "bindaxis a move -1 0" );
    cmd_execute_string( "bindaxis d move 1 0" );
    cmd_execute_string( "bindaxis pad_lstick move" );
    cmd_execute_string( "bindaxis mouse look" );
    cmd_execute_string( "bindaxis pad_rstick look 3 3" );

    input()->context_push( CTX_GAME );

    printf( "\n=== sb_input: bind -> cmd -> action proof ===\n" );
    printf( "W/S/A/D+SPACE move, UP also = forward, MOUSE1 attack, PAD_A jump,\n" );
    printf( "F plain bind, TAB toggle UI context (force-release), ESC quit\n" );
    printf( "axes: WASD+left stick -> [move], mouse+right stick -> [look], C writeconfig\n\n" );
    cmd_execute_string( "bindlist" );
    cmd_execute_string( "actionlist" );
    cmd_execute_string( "axislist" );
    printf( "\n" );

    bool ui_open = false;
    bool quit    = false;
    i64  prev_us = sys_tick_microseconds();

    while ( !quit && app()->pump_events() )
    {
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            /* Sandbox-owned keys first; everything else feeds the bind system the same
               way run_host does (keyboard + pad codes direct, mouse translated). */
            if ( ev.type == APP_EV_KEY_DOWN && !ev.data.key.repeat )
            {
                if ( ev.data.key.key == APP_KEY_ESCAPE )
                {
                    quit = true;
                }
                else if ( ev.data.key.key == APP_KEY_C )
                {
                    /* One file round-trips everything: seta cvars (in_mouse_sens...),
                       unbindall + bind lines, then our unbindaxisall + bindaxis lines. */
                    cmd_execute_string( "writeconfig test_input.cfg" );
                }
                else if ( ev.data.key.key == APP_KEY_TAB )
                {
                    ui_open = !ui_open;
                    if ( ui_open )
                        input()->context_push( CTX_UI );
                    else
                        input()->context_pop();
                    printf( "[ctx]   %s (active mask %08x)\n", ui_open ? "UI pushed -- gameplay gated"
                                                                       : "UI popped -- gameplay live",
                            input()->context_active() );
                }
                else
                {
                    cmd_bind_event( ( u32 )ev.data.key.key, true );
                }
            }
            else if ( ev.type == APP_EV_KEY_UP )
                cmd_bind_event( ( u32 )ev.data.key.key, false );
            else if ( ev.type == APP_EV_MOUSE_DOWN )
                cmd_bind_event( ( u32 )( APP_SRC_MOUSE1 + ev.data.mouse_btn.button ), true );
            else if ( ev.type == APP_EV_MOUSE_UP )
                cmd_bind_event( ( u32 )( APP_SRC_MOUSE1 + ev.data.mouse_btn.button ), false );
            else if ( ev.type == APP_EV_WIN_CLOSE )
                quit = true;
        }

        const i64 now_us = sys_tick_microseconds();
        const f32 dt     = ( f32 )( now_us - prev_us ) * 1e-6f;
        prev_us          = now_us;

        /* The host loop contract: pump the command buffer, THEN latch the frame. */
        cmd_pump();
        input()->frame( dt );

        /* Report edges (counts, so a sub-frame tap shows press=1 release=1) and state. */
        bool state_changed = false;
        for ( u32 i = 0; i < ARRAY_COUNT( s_acts ); ++i )
        {
            const u32 np = input()->pressed( s_acts[ i ].id );
            const u32 nr = input()->released( s_acts[ i ].id );

            if ( np )
                printf( "[edge]  PRESS   %-10s x%u\n", s_acts[ i ].name, np );
            if ( nr )
                printf( "[edge]  RELEASE %-10s x%u\n", s_acts[ i ].name, nr );
            if ( np || nr )
                state_changed = true;
        }
        if ( state_changed )
            print_state_line();

        /* [move] on change: WASD composite + stick, post-deadzone, clamped -1..1. */
        {
            static f32 pm_x = 0.0f, pm_y = 0.0f;

            f32 mx, my;
            input()->value2( s_move, &mx, &my );
            if ( mx != pm_x || my != pm_y )
            {
                printf( "[move]  (%+5.2f, %+5.2f)\n", mx, my );
                pm_x = mx;
                pm_y = my;
            }
        }

        /* [look] throttled: per-frame delta accumulates into a running total. */
        {
            static f32 acc_x = 0.0f, acc_y = 0.0f;
            static i32 cool  = 0;

            f32 lx, ly;
            input()->value2( s_look, &lx, &ly );
            acc_x += lx;
            acc_y += ly;

            if ( cool > 0 )
                cool--;
            if ( ( lx != 0.0f || ly != 0.0f ) && cool == 0 )
            {
                printf( "[look]  d=(%+7.2f, %+7.2f) total=(%+9.1f, %+9.1f)\n", lx, ly, acc_x, acc_y );
                cool = 15;    // ~8 prints/sec at the 8 ms pace
            }
        }

        sys_sleep_milliseconds( 8 );
    }

    app()->window_close( win );
    mod_system_exit();
    printf( "sb_input done.\n" );
    return 0;
}

/*============================================================================================*/
