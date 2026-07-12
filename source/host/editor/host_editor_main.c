/*==============================================================================================

    host_editor_main.c -- the developer "editor.exe" host.

    The real editor executable.  Same driving as sb_host_editor -- gui() as an OPTIONAL
    SERVICE under run_host_main: the host owns the borderless window (with the gui-drawn chrome
    shell), the rhi context, the loop, and the pacing; the descriptor wires gui's font / caps /
    debug, and the UI is emitted in on_gui (called only on dirty frames -- retained-cache skip).
    render draws the scene behind the gui composite (render path A); the host flushes gui over it.

    What makes this the HOST and not the sandbox: it parses launch params up front (host_common,
    pre-engine) -- -project/-module select a project DLL that the runtime loads at boot and the
    game framework runner drives; this editor is pure POLICY (Play/Stop/Pause/Step buttons ->
    game() session calls, one tick per frame).  Idle-sleep pacing is always on, but a live
    session suspends it via the run_host realtime gate (see editor_update) -- Play must tick
    in realtime; Stop returns the loop to blocking on OS input.
    Everything below the arg parse is the same modern stack sb_host_editor validates.

    Q/R/D are developer hotkeys read from the CONSOLE (terminal focus only, via RUN_HOST_CONSOLE)
    so they can't be fumbled from the editor window -- see editor_handle_shortcuts.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_BORDERLESS | RUN_HOST_EDITOR_SLEEP

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/sys/sys_host.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/gui/gui_api.h"
#include "runtime_modules/render/render_api.h"
#include "game/game_api.h"
#include "runtime/run_project.h"

#include "runtime/run_api.h"
#include "runtime/run_host.h"

#include "host/common/host_common.h"

MOD_USE_APP;
MOD_USE_RUN;
MOD_USE_GUI;
MOD_USE_GAME;

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static bool s_show_scene = true;    /* submit the scene rect behind the gui                   */
static bool s_show_stats;           /* frame-clock readout window                             */

static host_project_t s_proj;    /* resolved -project/-module; the runner owns the session */

/*==============================================================================================
    Editor Host Functions
==============================================================================================*/

static void
editor_ready( void )
{
    /* gui and run are static services here -- their gateways bind directly, no fetch needed. */
    printf( "[editor] ready\n" );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all  D=toggle sleep debug\n" );

    /* game() is the framework runner -- it resolves the project's stable api slot and owns
       the play session; this editor only issues session calls (Play/Stop/Pause/Step). */
    MOD_HOST_FETCH_API( game );
    if ( s_proj.present && game() && game()->project_bind( s_proj.name ) )
        printf( "[editor] project '%s' loaded -- use Play to start it\n", s_proj.name );
}

/* Developer hotkeys.  DELIBERATELY read the CONSOLE (sys_key_pressed, terminal focus only) rather
   than the editor window, so these destructive dev actions can't be fumbled while working in the
   UI.  In-window user shortcuts would instead read app()->key_pressed fenced by
   gui()->want_capture_keyboard -- see gui_api.h.  These three are console-gated on purpose. */

static void
editor_handle_shortcuts( void )
{
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[editor] Q -- quit\n" );
        if ( game() )
            game()->stop();    /* run_host_quit is final -- close_request never runs */
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[editor] R -- reload all\n" );
        mod_reload_all();
    }

    if ( sys_key_pressed( PLATFORM_KEY_D ) )
         run_host_sleep_debug_toggle();
}

/* Viewport scene feed -- the editor's per-frame render submission.  render()->draw_scene replays
   whatever is submitted here behind the gui composite (frame_cmd hand-off in run_host_main).  Kept
   separate from shell input above: this is the seam the real editor grows into (world tick ->
   visible-set cull -> submit).  A static rect proves the scene-under-gui path for now. */

static void
editor_submit_scene( void )
{
    if ( !render() || !s_show_scene )
        return;

    i32 ctx = run_host_ctx();
    i32 w = 0, h = 0;
    if ( rhi()->context_size( ctx, &w, &h ) && w > 0 && h > 0 )
    {
        const f32 slate[ 4 ] = { 0.16f, 0.20f, 0.28f, 1.0f };
        render()->submit_rect( ctx, ( f32 )w * 0.5f, ( f32 )h * 0.5f, ( f32 )w * 0.6f, ( f32 )h * 0.6f, slate );
    }
}

/*==============================================================================================
    Host Callbacks
==============================================================================================*/

/* Per-frame host update -- every frame, widget-free (widgets go in on_gui, which the retained
   cache may skip).  Editor shell input, then the viewport scene feed -- each in its own helper. */

static void
editor_update( f32 dt )
{
    editor_handle_shortcuts();
    editor_submit_scene();

    /* The one per-frame runner call -- a no-op while GAME_STOPPED.  View rebuilt every
       frame: surface size tracks resizes and a hot-reloaded project can never hold a
       stale handle.  This is also the future play-in-editor seam: swap render_ctx for a
       viewport context, no contract change. */
    if ( game() )
    {
        run_view_t view = {
            .version    = RUN_VIEW_VERSION,
            .render_ctx = run_host_ctx(),
        };
        app()->window_get_size( run_host_window(), &view.surface_w, &view.surface_h );

        game()->tick( dt, &view );
    }

    /* Realtime gate -- core to Play/Stop.  A live session (playing OR paused: the scene
       still draws every frame) suspends editor idle-sleep so the sim ticks without
       waiting on OS input; Stop returns the loop to blocking.  Re-derived from session
       state every frame so it survives every transition -- the Play/Stop buttons, the
       Q/close-request stops, and a project ending its own session. */
    run_host_realtime_set( game() && game()->state() != GAME_STOPPED );
}

/* UI emission -- dirty frames only.  The chrome shell is already emitted by run_host (first in
   this context's build); everything here lays out below its caption band. */

static void
editor_gui( f32 dt )
{
    UNUSED( dt );

    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "File" ) )
        {
            bool quit = false;
            if ( gui()->menu_item( "Quit", NULL, &quit ) )
                run_host_quit();
            gui()->menu_end();
        }
        if ( gui()->menu_begin( "View" ) )
        {
            gui()->menu_item( "Scene rect", NULL, &s_show_scene );
            gui()->menu_item( "Frame stats", NULL, &s_show_stats );
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

    f32 caption_h = gui()->viewport_caption_h( run_host_vp() );

    gui()->window_set_next_pos ( 40.0f, caption_h + 60.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 360.0f, 220.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Editor", GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* Project controls -- session calls into the game framework runner.  Play/Stop
           restart the session (the sample's score reset proves it), Pause freezes the sim
           while the scene keeps drawing, Step advances exactly one sim tick. */
        if ( s_proj.present && game() )
        {
            i32 st = game()->state();

            gui()->textf( "project: %s", s_proj.name );

            if ( gui()->button( st == GAME_STOPPED ? "Play" : "Stop" ) )
            {
                if ( st == GAME_STOPPED )
                    game()->play();
                else
                    game()->stop();
            }

            if ( st != GAME_STOPPED )
            {
                if ( gui()->button( st == GAME_PAUSED ? "Resume" : "Pause" ) )
                    game()->pause( st != GAME_PAUSED );

                if ( st == GAME_PAUSED && gui()->button( "Step" ) )
                    game()->step();
            }

            gui()->textf( "state  %s", st == GAME_PLAYING ? "PLAYING"
                                     : st == GAME_PAUSED  ? "PAUSED"
                                                          : "stopped" );
        }
        else
        {
            gui()->textf( "no project (-project <dir>)" );
        }

        gui()->separator();
        gui()->checkbox( "Scene rect (behind gui)", &s_show_scene );
    }
    gui()->window_end();

    if ( s_show_stats )
    {
        gui()->window_set_next_pos ( 420.0f, caption_h + 60.0f, GUI_COND_ONCE );
        gui()->window_set_next_size( 300.0f, 160.0f, GUI_COND_ONCE );
        if ( gui()->window_begin( "Frame Stats", GUI_WIN_NONE ) )
        {
            gui()->stack();
            const run_clock_t* clk = run()->clock();
            gui()->textf( "frame  %llu", ( unsigned long long )clk->frame_number );
            gui()->textf( "time   %.1fs", clk->app_time );
            gui()->textf( "dt     %.2f ms", clk->dt * 1000.0f );
        }
        gui()->window_end();
    }
}

/* Main-window X pressed -- the veto point for "unsaved changes" flows.  Stop the session
   if one is active, then allow the close. */
static bool
editor_close_request( void )
{
    printf( "[editor] close requested -- allowing\n" );
    if ( game() )
        game()->stop();
    return true;
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( core   ),    /* cvars, logging, memory arenas -- static       */
    RUN_SERVICE( app    ),    /* window, OS pump                               */
    RUN_SERVICE( rhi    ),    /* GPU backend -- static service                 */
    RUN_SERVICE( draw   ),    /* immediate primitives -- render's draw backend */
    RUN_SERVICE( gui    ),    /* immediate mode GUI -- OPTIONAL static service */
    RUN_MODULE ( render ),    /* scene frame owner -- gui composites over it   */
    RUN_MODULE ( game   ),    /* game framework runner -- drives project DLLs   */
    { 0 }
};

static const gui_forward_caps_t k_gui_caps = {
    .keyboard_nav = true,
    .tables       = true,
    .docking      = true,
};

static const run_gui_desc_t k_gui_desc = {
    .font  = GUI_FONT_ROBOTO_16,
    .caps  = &k_gui_caps,
    .clear = { 0.10f, 0.10f, 0.12f, 1.00f },
    .debug = true,            /* P/O/F10 overlays, I idle skip, etc. */
};

/*
    host_game.exe   -project F:\orb\my_project     -> plays my_project.dll
    host_game.exe   -module  sample_game           -> plays sample_game.dll from engine bin
    host_editor.exe -project F:\orb\my_project     -> loads it; Play/Stop drives it
*/

int
main( int argc, char** argv )
{
    /* Pre-engine launch params (host_common): -project/-module select the game project
       DLL, -dev arms idle-sleep pacing.  An absent project is fine -- the editor runs
       standalone; an INVALID one (bad path, missing dll, reserved name) is a hard error
       so a typo can't silently launch a projectless editor. */
    launch_params_t params;
    host_args_parse( argc, argv, &params );

    char err[ 512 ];
    if ( !host_resolve_project( &params, &s_proj, err, sizeof( err ) ) )
    {
        fprintf( stderr, "[editor] %s\n", err );
        return 1;
    }

    /* The editor is always dev mode: idle-sleep is unconditional.  A live play session
       suspends it through the realtime gate (editor_update), not by dropping the flag. */
    u32 flags = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_BORDERLESS | RUN_HOST_EDITOR_SLEEP;

    const run_host_desc_t desc = {
        .name             = "orb editor",
        .flags            = flags,
        .loop_mode        = RUN_LOOP_RUN,
        .window_width     = 1600,
        .window_height    = 900,
        .modules          = k_modules,
        .gui              = &k_gui_desc,
        .project_name     = s_proj.present ? s_proj.name : NULL,
        .project_dir      = s_proj.present ? s_proj.dir  : NULL,
        .on_ready         = editor_ready,
        .on_update        = editor_update,
        .on_gui           = editor_gui,
        .on_close_request = editor_close_request,
    };

    return run_host_main( &desc, argc, argv );
}

// clang-format on
/*============================================================================================*/
