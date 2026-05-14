/*==============================================================================================

    host/game/game_main.c

    The retail "game.exe" host.  Optimized for one thing: running the simulation.

    Context (specific to this host)
    ───────────────────────────────
        - frame-rate throttler          (target_fps)
        - "game world" handle           (here just a bool — a real engine has a world*)
        - primary window handle         (stub here — the platform layer would own a HWND)
        - hot-reload poll               (always on, even in retail; cheap if no DLLs change)

    Behavior
    ────────
        Loads core (Tier 1) + render/audio/physics (Tier 2) + game framework.
        Hides console output behind the core logger and runs until on_frame returns false.
        Pressing close-button in a real implementation flips the should_quit flag.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"

// #include "host/common/host_common.h"    // CLI parsing and platform-specific early setup

#include "engine/mod/mod.h"        // mod_<functions>
#include "engine/mod/mod_api.h"    // api_access macros

#include "engine/core/core.h"
#include "engine/sys/sys.h"
#include "engine/app/app.h"    // app_loop_t, app_loop_run()

#include "runtime_module/render/render_api.h"
#include "runtime_module/audio/audio_api.h"
#include "runtime_module/physics/physics_api.h"

#include "game/game_api.h"

/* Consumer-side storage + binder — see runtime_main.c for explanation. */
MOD_DEFINE_API_PTR( render_api_t, render );
MOD_DEFINE_API_PTR( audio_api_t, audio );
MOD_DEFINE_API_PTR( physics_api_t, physics );
MOD_DEFINE_API_PTR( game_api_t, game );

/*==============================================================================================
    Per-host context — this is the "bucket" the host owns and passes to its frame fn.
==============================================================================================*/

typedef struct game_host_ctx_s
{
    bool  should_quit;
    int   frames;
    int   max_frames;    /* sanity cap so the demo exits */
    void* window_handle; /* HWND / NSWindow* / xcb_window_t — stubbed for the example */

} game_host_ctx_t;

/*==============================================================================================
    Per-frame callback — this is what app_loop_run drives.
==============================================================================================*/

static bool
on_frame( void* user, float dt )
{
    UNUSED( dt );
    game_host_ctx_t* ctx = ( game_host_ctx_t* )user;

    /* Hot-reload check first so a freshly-recompiled DLL takes effect THIS frame. */
    mod_check_reloads();

    ctx->frames++;
    if ( ctx->frames >= ctx->max_frames )
        ctx->should_quit = true;

    return !ctx->should_quit;
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    // host_common_early_setup();
    // 
    // /* 1. Boot the module system, register static modules, load dynamic ones, and initialize all. */
    // 
    // mod_system_init();
    // 
    // // 2. Register the mandatory foundation
    // 
    // mod_static_load( "sys", sys_get_mod_api() );
    // mod_static_load( "core", core_get_mod_api() );
    // 
    // // 3. Register the specific A-La-Carte services for THIS host
    // mod_static_load( "app", app_get_mod_api() );
    // // mod_static_load( "jobs", rt_jobs_get_api() );
    // // mod_static_load( "input", rt_input_get_api() );
    // 
    // // 4. Setup Config and Run
    // runtime_config_t config = { 0 };
    // host_common_parse_args( argc, argv, &config );
    // config.project_dll = "amberfall";
    // 
    // // 5. Hand off to the agnostic loop
    // return runtime_host_run( &config );

    // if ( !mod_load( render ) )
    //     return 1;
    // if ( !mod_load( audio ) )
    //     return 1;
    // if ( !mod_load( physics ) )
    //     return 1;
    // if ( !mod_load( game ) )
    //     return 1;
    // 
    // if ( !mod_init_all() )
    // {
    //     fprintf( stderr, "init failed: %s\n", mod_last_error() );
    //     mod_system_exit();
    //     return 1;
    // }

    // const core_api_t* core_api_test = core_api();
    // UNUSED( core_api_test );
    // 
    // /* Bind host-side gateway pointers (no-op in BUILD_STATIC). */
    // if ( !HOST_FETCH_API( render_api_t, render ) )
    //     return 1;
    // if ( !HOST_FETCH_API( audio_api_t, audio ) )
    //     return 1;
    // if ( !HOST_FETCH_API( physics_api_t, physics ) )
    //     return 1;
    // if ( !HOST_FETCH_API( game_api_t, game ) )
    //     return 1;
    // 
    // mod_list_all();
    // 
    // /* --- start the simulation ------------------------------------------- */
    // 
    // game_api()->on_start();
    // 
    // /* --- enter the frame loop ------------------------------------------- */
    // 
    // game_host_ctx_t ctx = {
    //     .should_quit   = false,
    //     .frames        = 0,
    //     .max_frames    = 240, /* ~4 seconds at 60 FPS for the demo */
    //     .window_handle = NULL,
    // };
    // 
    // app_loop_t loop = {
    //     .on_frame   = on_frame,
    //     .user       = &ctx,
    //     .target_fps = 60,
    // };
    // 
    // app_loop_run( &loop );
    // 
    // /* --- shutdown ------------------------------------------------------- */
    // 
    // game_api()->on_stop();
    // core_api()->log( "game_host: ran %d frames, final score = %d", ctx.frames, game_api()->score() );
    // 
    // mod_system_exit();
    // return 0;
}

/*============================================================================================*/