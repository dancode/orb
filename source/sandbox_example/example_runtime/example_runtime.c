/*==============================================================================================

    sandbox_example/example_runtime/example_runtime.c — WINDOWED RUNTIME shape.

    Minimal host that exercises the full runtime stack below the game framework:
    engine (sys + app) + render module. Use this as a starting point for testing
    new runtime services or modules without pulling in game or editor layers.

    k_modules[] is the single declaration of intent. Loading RUN_SERVICE(app) causes
    the host to create a window and pump OS events. Loading RUN_MODULE(render) causes
    the host to drive begin_frame / draw_frame / end_frame each tick.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/app/app.h"
#include "engine/core/core.h"

#include "runtime_modules/render/render.h"
#include "runtime/host.h"

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
runtime_ready( void )
{
    /* Both app and render are initialized and the window exists - safe to call any API. */
    render_api()->set_clear_color( 0.08f, 0.10f, 0.18f );
    printf( "[example_runtime] ready - press escape to quit, R to reload modules\n" );
}

static void
runtime_update( f32 dt )
{
    UNUSED( dt );

    if ( app_api()->key_pressed( APP_KEY_ESCAPE ) )
    {
        run_host_quit();
        return;
    }

    if ( app_api()->key_pressed( APP_KEY_R ) )
    {
        printf( "[example_runtime] reloading all modules\n" );
        mod_reload_all();
    }
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( app    ),   /* windowing, input — always statically linked */
    RUN_SERVICE( core ),     /* logging, time, file I/O — always statically linked */
    RUN_MODULE ( render ),   /* renderer — hot-reloadable DLL in dynamic builds */
    { 0 },
};

static const run_host_desc_t k_desc = {
    .name      = "example_runtime",
    .flags     = RUN_HOST_HOT_RELOAD,
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
