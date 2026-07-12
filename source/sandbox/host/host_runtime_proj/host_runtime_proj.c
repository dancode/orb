/*==============================================================================================

    sb_host_runtime_proj.c -- the BYPASS PROOF: a custom host driving the project contract
    (runtime/run_project.h) directly, with NO game framework.

    The invariant this sandbox enforces: the project contract is runtime-level.  A
    project DLL can depend on just core + render and be run by any host willing to drive
    the vtable itself -- the game framework runner is the standard driver, never a
    requirement.  Note k_modules[] below: the game module is not loaded at all.

    The drive is the documented direct pattern from run_project.h: a ~10-line fixed-step
    accumulator, on_sim xN, on_frame, on_draw with the interpolation alpha.  The runtime
    loads and hot-reloads proj_runtime.dll (project_name in the descriptor) exactly as it
    does for host_game -- it never calls into it; this host does.

    Dev keys Q/R read the CONSOLE (terminal focus only) -- same policy as host_game.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOT_RELOAD

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
#include "runtime/run_project.h"

#include "runtime/runtime_api.h"
#include "runtime/runtime_host.h"

MOD_USE_APP;
MOD_USE_RUN;

// clang-format off
/*==============================================================================================
    Host state -- the direct drive: vtable + fixed-step accumulator, nothing else.
==============================================================================================*/

#define SB_PROJECT       "proj_runtime"
#define SB_FIXED_HZ      60.0f
#define SB_MAX_SIM_STEPS 4        /* stall guard -- drop time rather than spiral */

static const run_project_api_t* s_project = NULL;   /* stable api slot; live across reloads */
static f32                      s_acc     = 0.0f;   /* fixed-step accumulator               */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
raw_host_ready( void )
{
    s_project = ( const run_project_api_t* )mod_get_api( SB_PROJECT );
    if ( !s_project )
    {
        fprintf( stderr, "[sb_host_runtime_proj] project '%s' has no api\n", SB_PROJECT );
        run_host_quit();
        return;
    }

    printf( "[sb_host_runtime_proj] driving '%s' directly -- no game framework loaded\n", SB_PROJECT );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all\n" );

    s_project->on_start();
}

static void
raw_host_update( f32 dt )
{
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[sb_host_runtime_proj] Q -- quit\n" );
        if ( s_project )
            s_project->on_stop();
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[sb_host_runtime_proj] R -- reload all\n" );
        mod_reload_all();
    }

    if ( !s_project )
        return;

    run_view_t view = {
        .version    = RUN_VIEW_VERSION,
        .render_ctx = run_host_ctx(),
    };
    app()->window_get_size( run_host_window(), &view.surface_w, &view.surface_h );

    /* The direct drive from run_project.h -- what the game framework runner does for
       standard hosts, spelled out to prove no framework is required. */
    const f32 fixed_dt = 1.0f / SB_FIXED_HZ;

    s_acc += dt;
    if ( s_acc > fixed_dt * SB_MAX_SIM_STEPS )
        s_acc = fixed_dt * SB_MAX_SIM_STEPS;

    while ( s_acc >= fixed_dt )
    {
        s_project->on_sim( fixed_dt );
        s_acc -= fixed_dt;
    }

    s_project->on_frame( dt, &view );
    s_project->on_draw( s_acc / fixed_dt, &view );
}

static bool
raw_host_close_request( void )
{
    if ( s_project )
        s_project->on_stop();

    return true;
}

/*==============================================================================================
    Host descriptor -- note: no game module anywhere.
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( core   ),
    RUN_SERVICE( app    ),
    RUN_SERVICE( rhi    ),
    RUN_SERVICE( draw   ),
    RUN_MODULE ( render ),
    { 0 }
};

int
main( int argc, char** argv )
{
    const run_host_desc_t desc = {
        .name             = "sb_host_runtime_proj",
        .flags            = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD,
        .loop_mode        = RUN_LOOP_RUN,
        .window_width     = 1280,
        .window_height    = 720,
        .modules          = k_modules,
        .project_name     = SB_PROJECT,    /* loaded from the exe dir */
        .on_ready         = raw_host_ready,
        .on_update        = raw_host_update,
        .on_close_request = raw_host_close_request,
    };

    return run_host_main( &desc, argc, argv );
}

// clang-format on
/*============================================================================================*/
