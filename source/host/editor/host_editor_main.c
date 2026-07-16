/*==============================================================================================

    host_editor_main.c -- the developer "editor.exe" host.

    The real editor executable.  gui() as an OPTIONAL SERVICE under run_host_main: the host
    owns the borderless window (with the gui-drawn chrome shell), the rhi context, the loop,
    and the pacing; the descriptor wires gui's font / caps / debug.

    The editor FRAMEWORK is the editor static service (source/editor): menu bar, dockspace,
    the official editor windows, and the scene viewport's play-in-editor plumbing.  This host
    is pure POLICY -- it parses launch params up front (host_common, pre-engine; -project /
    -module select the project DLL the runtime loads at boot), forwards update/build_gui to
    editor(), and drives the session: one game()->tick per frame whose run_view_t carries the
    editor's scene viewport target (editor()->view_ctx) instead of the window's swapchain --
    the game renders INTO the docked Viewport panel.

    Idle-sleep pacing is always on, but a live session suspends it via the run_host realtime
    gate, and the same liveness pins gui's force-redraw so panels track the sim every frame.

    Q/R/D are developer hotkeys read from the CONSOLE (terminal focus only, via
    RUN_HOST_CONSOLE) so they can't be fumbled from the editor window.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_WINDOWED | RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_BORDERLESS | RUN_HOST_EDITOR_SLEEP

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

#include "editor/editor_api.h"

#include "host/common/host_common.h"

MOD_USE_APP;
MOD_USE_RUN;
/* game's api pointer is owned by the editor service (editor.c MOD_USE_GAME) -- this TU
   reads the same global through game_api.h's extern. */

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static host_project_t s_proj;    /* resolved -project/-module; the runner owns the session */

/*==============================================================================================
    Editor : Ready To Init Callback
==============================================================================================*/

static void
editor_ready( void )
{
    /* gui, run, and editor are static services here -- their gateways bind directly.
       game() was fetched by the editor service's dep-ordered init. */
    printf( "[editor] ready\n" );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all  D=toggle sleep debug\n" );

    /* game() is the framework runner -- it resolves the project's stable api slot and owns
       the play session; this host only issues session calls (Play/Stop/Pause/Step). */

    if ( s_proj.present )
    {
        if ( game() && game()->project_bind( s_proj.name ) )
        {
            printf( "[editor] project '%s' loaded -- use Play to start it\n", s_proj.name );
            editor()->set_project( s_proj.name );
        }
    }
}

/*==============================================================================================
    Host Callbacks
==============================================================================================*/

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
        editor()->shutdown();  /* free GPU-backed editor resources before device teardown */
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

/*==============================================================================================
    Host Callbacks : General Tick (State Update + Render)
==============================================================================================*/

/* Per-frame host update -- every frame, widget-free (widgets go in on_gui, which the retained
   cache may skip).  Shell input, the session tick, then the editor's own maintenance. */

static void
editor_update( f32 dt )
{
    editor_handle_shortcuts();

    /* The one per-frame runner call -- a no-op while GAME_STOPPED.  View rebuilt every
       frame: the render target is the editor's SCENE VIEWPORT (a docked panel), not the
       window swapchain -- this is the play-in-editor seam run_project.h names.  -1 =
       headless (viewport hidden or not yet sized); the project skips its draws. */

    if ( game() )
    {
        run_view_t view = {
            .version    = RUN_VIEW_VERSION,
            .render_ctx = editor()->view_ctx(),
        };
        editor()->view_size( &view.surface_w, &view.surface_h );
        game()->tick( dt, &view );
    }

    /* Editor maintenance AFTER the tick filled the target's submission bucket and BEFORE
       the gui emit bakes the target's texture index (the flip lives in here). */
    editor()->update( dt );

    /* Realtime gate -- core to Play/Stop.  A live session (playing OR paused: the scene
       still draws every frame) suspends editor idle-sleep so the sim ticks without
       waiting on OS input; Stop returns the loop to blocking.  The same liveness pins
       gui's force-redraw: the viewport image changes every frame, so clean-frame skips
       would freeze it.  Re-derived from session state every frame so it survives every
       transition -- buttons, Q/close-request stops, a project ending its own session. */

    bool live = game() && game()->state() != GAME_STOPPED;
    run_host_realtime_set( live );
    gui()->set_force_redraw( live );
}

/*==============================================================================================
    Host Callbacks : GUI Emit
==============================================================================================*/

/* UI emission -- dirty frames only.  The chrome shell is already emitted by run_host (first
   in this context's build); the editor framework builds everything else. */

static void
editor_gui( f32 dt )
{
    editor()->build_gui( dt );
}

/*==============================================================================================
    Host : Close Request (X pressed)
==============================================================================================*/

/* Main-window X pressed -- the veto point for "unsaved changes" flows.  Stop the session
   if one is active, then allow the close. */
static bool
editor_close_request( void )
{
    printf( "[editor] close requested -- allowing\n" );
    if ( game() )
         game()->stop();

    editor()->shutdown();    /* free GPU-backed editor resources before device teardown */
    return true;
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( rhi    ),    /* GPU backend -- static service                    */
    RUN_SERVICE( draw   ),    /* immediate primitives -- render's draw backend    */
    RUN_SERVICE( gui    ),    /* immediate mode GUI -- OPTIONAL static service    */
    RUN_MODULE ( render ),    /* scene frame owner -- draws the viewport targets  */
    RUN_MODULE ( game   ),    /* game framework runner -- drives project DLLs     */
    RUN_SERVICE( editor ),    /* editor framework -- shell, windows, viewport     */
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
    u32 flags = RUN_HOST_WINDOWED | RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_BORDERLESS | RUN_HOST_EDITOR_SLEEP;

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
