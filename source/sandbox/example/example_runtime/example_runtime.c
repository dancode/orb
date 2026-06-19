/*==============================================================================================

    sandbox_example/example_runtime/example_runtime.c

    A minimal runtime host. The smallest viable shape for booting the engine stack
    without pulling in game or editor layers above it.

    What the runtime host actually is
    ---------------------------------
    The "runtime host" is the executable that owns main(). It boots the module system,
    drives the frame loop, and applies hot-reload swaps at safe points. Everything else
    (rendering, simulation, gameplay, tools) lives inside modules behind APIs.

    This file is the smallest expression of that pattern: declare which modules to load,
    install two callbacks, hand the descriptor to run_host_main().


    Engine baseline (auto-loaded by run_host_main)
    ----------------------------------------------
        sys   — OS abstractions (clock, files, threads, DLL loader)
        rs    — type reflection registry; auto-wires DLL load/unload events
        run   — authoritative frame clock (app_time, dt, frame_number)

    These are always-on. k_modules[] only needs to declare modules ABOVE this baseline.


    Modules declared by this host
    -----------------------------
        core   — cvars, logging, memory arenas       (static service)
        app    — windowing, input, OS event pump     (static service; presence -> windowed)
        rhi    — Vulkan render hardware interface    (static service; inits after window opens)
        render — renderer front-end                  (hot-reloadable DLL in dynamic builds)


    Frame loop (driven by run_host_main)
    ------------------------------------
        pump_events  -> if windowed
        clock_update -> dt = run()->clock()->dt   (capped, time-scaled)
        on_update    -> this file's per-frame hook
        render       -> begin_frame / draw_frame / end_frame, if render is loaded
        hot_reload   -> mod_check_reloads + flush at the single safe point per frame


    Where higher engine layers will plug in
    ---------------------------------------
    The runtime host is the convergence point for everything above it. Each layer adds
    entries to k_modules[] and (optionally) does work in on_ready / on_update:

        runtime_service/  - input, timing, asset i/o            (static services)
        runtime_modules/  - render, audio, physics, animation   (hot-reload DLLs)

        game/framework/   - world, entity, component, actor     (static; game-side base)
        game_service/     - persistent game-wide services       (static)
        game_modules/     - hot-reload gameplay DLLs

        editor/           - editor framework                    (only in editor hosts)
        editor_service/   - editor-wide persistent services
        editor_modules/   - hot-reload editor tooling DLLs

        project/<name>/   - project-specific modules and bootstrap (the actual game)

    A game executable is the same shape as this file: a different k_modules[], a different
    on_ready that boots the world, a different on_update that ticks simulation. The host
    machinery underneath does not change.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_api.h"
#include "engine/app/app_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_modules/render/render_api.h"
#include "sandbox/reflect/example_reflect/example_reflect_api.h"
#include "runtime/runtime_api.h"
#include "runtime/runtime_host.h"

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
runtime_ready( void )
{
    /* Called once after mod_init_all() and window creation. All APIs in k_modules[]
       are live. Use for one-time setup: register cvars, install default settings,
       hand initial state to modules. */

    render()->set_clear_color( 0, 0.08f, 0.10f, 0.18f, 1.0f );

    printf( "[example_runtime] ready - ESC to quit, R to reload all DLLs\n" );

    /* Future: bootstrap higher layers here.
       - game_world_create() once a game/framework module is loaded
       - editor_open_default_layout() in an editor host
       - project_init() to hand control to project/<name>/ code */
}

static void
runtime_update( f32 dt )
{
    UNUSED( dt );

    if ( app()->key_pressed( APP_KEY_ESCAPE ) )
    {
        run_host_quit();
        return;
    }

    if ( app()->key_pressed( APP_KEY_R ) )
    {
        printf( "[example_runtime] reloading all dynamic modules\n" );
        mod_reload_all();
    }

    /* Future per-frame work belongs here, between clock update and render:
       - input sampling -> input_api()->snapshot()
       - simulation tick -> world_api()->tick( dt )
       - gameplay scripts -> script_api()->run_frame( dt )
       Rendering is driven automatically by the host when render() is live. */
}

/*==============================================================================================
    Host descriptor

    k_modules[] is the single declaration of what this host is. Adding a service or module
    here is the only step needed to bring it online — the host picks up windowed mode,
    RHI init, and the render loop automatically based on which entries are present.
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( core   ),   /* cvars, logging, memory arenas — static                       */
    RUN_SERVICE( app    ),   /* windowing + input — static; presence enables windowed mode    */
    RUN_SERVICE( rhi    ),   /* Vulkan RHI — static; inits after window_open                  */
    RUN_MODULE ( render ),   /* renderer front-end — DLL in dynamic builds, static otherwise  */
    RUN_MODULE( example_reflect ),   /* example  module to test reflection */

    /* Future entries (commented out until those layers exist):
        RUN_SERVICE( input      ),
        RUN_SERVICE( asset      ),
        RUN_MODULE ( audio      ),
        RUN_MODULE ( physics    ),
        RUN_MODULE ( animation  ),

        RUN_SERVICE( world      ),    // game/framework
        RUN_MODULE ( gameplay   ),    // game_modules / project hot-reload DLLs
    */


    { 0 },
};

static const run_host_desc_t k_desc = {
    .name      = "example_runtime",
    .flags     = RUN_HOST_HOT_RELOAD | RUN_HOST_CONSOLE,
    .loop_mode = RUN_LOOP_RUN,
    .modules   = k_modules,
    .on_ready  = runtime_ready,
    .on_update = runtime_update,
};

int
main( int argc, char** argv )
{
    return run_host_main( &k_desc, argc, argv );
}

/*============================================================================================*/
