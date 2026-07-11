/*==============================================================================================

    host_game_main.c -- the "game.exe" host: engine + runtime + PROJECT GAME DLL.

    The runtime shape of the modular architecture: this exe owns the window, the rhi
    context, and the loop (run_host_main); the GAME is a project DLL selected at launch
    and loaded by the runtime at boot:

        host_game.exe -project <dir>                 loads <dir>/bin/<name>.dll (name = dir basename)
        host_game.exe -module sample_game            loads sample_game.dll from the exe dir
        host_game.exe -project <dir> -module <name>  -module overrides the basename

    The project implements game/game_project.h; this host fetches the vtable once in
    on_ready (mod_get_api returns the stable api slot -- live across hot-reloads) and
    drives it every frame: on_start at ready, on_update( dt, ctx ) per frame, on_stop on
    close.  The ctx hands the project its render target and surface size each update.

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
#include "game/game_project.h"

#include "runtime/runtime_api.h"
#include "runtime/runtime_host.h"

#include "host/common/host_common.h"

MOD_USE_APP;
MOD_USE_RUN;

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static host_project_t            s_proj;             /* resolved -project/-module            */
static const game_project_api_t* s_project = NULL;   /* stable api slot; live across reloads */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
game_host_ready( void )
{
    /* The stable api slot: the mod system rewrites its contents on every hot-reload, so
       this pointer never needs refreshing. */
    s_project = ( const game_project_api_t* )mod_get_api( s_proj.name );
    if ( !s_project )
    {
        fprintf( stderr, "[host_game] project '%s' has no api\n", s_proj.name );
        run_host_quit();
        return;
    }

    printf( "[host_game] running project '%s'\n", s_proj.name );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all\n" );

    s_project->on_start();
}

static void
game_host_update( f32 dt )
{
    /* Developer hotkeys -- CONSOLE input on purpose (terminal focus only; can't be
       fumbled from the game window).  See host_editor_main.c for the full rationale. */
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[host_game] Q -- quit\n" );
        if ( s_project )
            s_project->on_stop();
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[host_game] R -- reload all\n" );
        mod_reload_all();
    }

    /* Drive the project.  The ctx is rebuilt every frame -- surface size tracks resizes
       and a hot-reloaded project can never hold a stale handle. */
    if ( s_project )
    {
        game_project_ctx_t ctx = {
            .version    = GAME_PROJECT_CTX_VERSION,
            .render_ctx = run_host_ctx(),
        };
        app()->window_get_size( run_host_window(), &ctx.surface_w, &ctx.surface_h );

        s_project->on_update( dt, &ctx );
    }
}

/* Window X pressed: stop the project, then allow the close.  (Q-quit calls on_stop
   itself; the module's exit() during mod_system_exit is the backstop either way.) */

static bool
game_host_close_request( void )
{
    if ( s_project )
         s_project->on_stop();

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
    RUN_MODULE ( game   ),    /* gameplay framework -- project DLLs build on it */
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
