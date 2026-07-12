/*==============================================================================================

    host_game_main.c -- the "game.exe" host: engine + runtime + PROJECT GAME DLL.

    The runtime shape of the modular architecture: this exe owns the window, the rhi
    context, and the loop (run_host_main); the GAME is a project DLL selected at launch
    and loaded by the runtime at boot:

        host_game.exe -project <dir>                 loads <dir>/bin/<name>.dll (name = dir basename)
        host_game.exe -module sample_game            loads sample_game.dll from the exe dir
        host_game.exe -project <dir> -module <name>  -module overrides the basename

    The project implements runtime/run_project.h, and the game framework runner drives
    it -- this host is pure POLICY: bind + play at ready, one game()->tick( dt, view )
    per frame (fixed-step sim, pacing, and the play session live in the runner), stop on
    close.  The view hands the project its render target and surface size every frame.

    Dev keys Q/R read the CONSOLE (terminal focus only) so they can't be fumbled from the
    game window -- same policy as host_editor.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/sys/sys_host.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_modules/render/render_api.h"
#include "game/game_api.h"
#include "runtime/run_project.h"

#include "runtime/run_api.h"
#include "runtime/run_host.h"

#include "host/common/host_common.h"

MOD_USE_APP;
MOD_USE_RUN;
MOD_USE_GAME;

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static host_project_t s_proj;    /* resolved -project/-module */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
game_host_ready( void )
{
    /* Hand the project to the runner: bind resolves its stable api slot (live across
       hot-reloads), play starts the session.  This host's policy is "play at boot". */
    if ( !MOD_HOST_FETCH_API( game ) || !game()->project_bind( s_proj.name ) )
    {
        fprintf( stderr, "[host_game] project '%s' has no api\n", s_proj.name );
        run_host_quit();
        return;
    }

    printf( "[host_game] running project '%s'\n", s_proj.name );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all\n" );

    game()->play();
}

static void
game_host_update( f32 dt )
{
    /* Developer hotkeys -- CONSOLE input on purpose (terminal focus only; can't be
       fumbled from the game window).  See host_editor_main.c for the full rationale. */
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[host_game] Q -- quit\n" );
        game()->stop();
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[host_game] R -- reload all\n" );
        mod_reload_all();
    }

    /* The one per-frame runner call.  The view is rebuilt every frame -- surface size
       tracks resizes and a hot-reloaded project can never hold a stale handle.  The
       runner owns everything else: fixed-step sim, interpolation alpha, play state. */

    run_view_t view = {
        .version    = RUN_VIEW_VERSION,
        .render_ctx = run_host_ctx(),
    };
    app()->window_get_size( run_host_window(), &view.surface_w, &view.surface_h );

    game()->tick( dt, &view );
}

/* Window X pressed: stop the session, then allow the close.  (Q-quit stops it too; the
   project module's own exit() during mod_system_exit is the backstop either way.) */

static bool
game_host_close_request( void )
{
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
    RUN_MODULE ( render ),    /* scene frame owner                             */
    RUN_MODULE ( game   ),    /* game framework runner -- drives project DLLs   */
    { 0 }
};

/*
    host_game.exe   -project F:\orb\my_project     -> plays my_project.dll
    host_game.exe   -module  sample_game           -> plays sample_game.dll from engine bin
    host_editor.exe -project F:\orb\my_project     -> loads it; Play/Stop drives it
*/

int
main( int argc, char** argv )
{
    launch_params_t params;
    host_args_parse( argc, argv, &params );

    char err[ 512 ];
    if ( !host_resolve_project( &params, &s_proj, err, sizeof( err ) ) )
    {
        fprintf( stderr, "[host_game] %s\n", err );
        return 1;
    }
    if ( !s_proj.present )
    {
        fprintf( stderr, "usage: host_game.exe -project <dir> [-module <name>] [-dev]\n"
                         "       host_game.exe -module <name>\n" );
        return 1;
    }

    u32 flags = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD;

    const run_host_desc_t desc = {
        .name             = s_proj.name,
        .flags            = flags,
        .loop_mode        = RUN_LOOP_RUN,
        .window_width     = 1280,
        .window_height    = 720,
        .modules          = k_modules,
        .project_name     = s_proj.name,
        .project_dir      = s_proj.dir,
        .on_ready         = game_host_ready,
        .on_update        = game_host_update,
        .on_close_request = game_host_close_request,
    };

    return run_host_main( &desc, argc, argv );
}

// clang-format on
/*============================================================================================*/
