/*==============================================================================================

    host_editor_main.c -- the developer "editor.exe" host.

    A configuration/launch shell, deliberately shaped like host_game_main.c: it parses
    launch params (host_common, pre-engine), declares the module stack, boots the runtime
    through run_host_main, and forwards the loop callbacks.  Everything editor-specific --
    the shell UI, the scene viewport, project bind, the session tick, session verbs --
    lives in the editor static service (source/editor); this host never touches game().

    The runtime doesn't know the editor exists: the editor is just another service in
    k_modules[], and the host is the messenger between run_host's callbacks and editor().
    Keeping the host this thin keeps host_game and host_editor from diverging -- the
    editor host is the game host plus one service and a borderless window.

    Idle-sleep pacing is always on; the editor service suspends it via the run_host
    realtime gate while a session is live (and pins gui force-redraw the same way).

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

#include "game/game_api.h" /* RUN_MODULE( game ) decl in monolithic builds */

#include "runtime/run_api.h"
#include "runtime/run_host.h"

#include "editor/editor_api.h"

#include "host/common/host_common.h"

MOD_USE_APP;
MOD_USE_RUN;

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static host_project_t s_proj;    /* resolved -project/-module; the editor owns the session */

/*==============================================================================================
    Editor : Ready To Init Callback
==============================================================================================*/

static void
editor_ready( void )
{
    printf( "[editor] ready\n" );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all  D=toggle sleep debug\n" );

    /* Hand the loaded project to the editor -- it binds the game runner and owns the
       session from here (Play/Stop/Pause/Step live in its Game window; the dll dir feeds
       Play Standalone's host_game.exe launch args). */
    if ( s_proj.present && !editor()->project_bind( s_proj.name, s_proj.dir ) )
        fprintf( stderr, "[editor] project '%s' has no api\n", s_proj.name );
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
        editor()->shutdown();    /* stop session + free GPU resources before device teardown */
        run_host_quit();         /* final -- close_request never runs */
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

/* Per-frame host update -- every frame, widget-free (widgets go in on_gui, which the retained
   cache may skip).  Shell input, then everything editor: session tick, viewport, pacing gates. */

static void
editor_update( f32 dt )
{
    editor_handle_shortcuts();
    editor()->update( dt );
}

/* UI emission -- dirty frames only.  The chrome shell is already emitted by run_host (first
   in this context's build); the editor framework builds everything else. */

static void
editor_gui( f32 dt )
{
    editor()->build_gui( dt );
}

/* Main-window X pressed -- the veto point for "unsaved changes" flows.  Stop the session
   and free editor GPU resources, then allow the close. */

static bool
editor_close_request( void )
{
    printf( "[editor] close requested -- allowing\n" );
    editor()->shutdown();
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
    RUN_SERVICE( editor ),    /* editor framework -- shell, session, viewport     */
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

int
main( int argc, char** argv )
{
    /* Pre-engine launch params (host_common; cheat sheet in host_common.h): -project/-module
       select the game project DLL, -dev arms idle-sleep pacing.  An absent project is fine --
       the editor runs standalone; an INVALID one (bad path, missing dll, reserved name) is a
       hard error so a typo can't silently launch a projectless editor. */
    launch_params_t params;
    host_args_parse( argc, argv, &params );

    char err[ 512 ];
    if ( !host_resolve_project( &params, &s_proj, err, sizeof( err ) ) )
    {
        fprintf( stderr, "[editor] %s\n", err );
        return 1;
    }

    /* The editor is always dev mode: idle-sleep is unconditional.  A live play session
       suspends it through the realtime gate (editor service), not by dropping the flag. */
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
