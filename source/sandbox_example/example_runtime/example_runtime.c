/*==============================================================================================

    sandbox_example/example_runtime/example_runtime.c — WINDOWED RUNTIME shape.

    Minimal host that exercises the full runtime stack below the game framework:
    engine (sys + app) + render module. Use this as a starting point for testing
    new runtime services or modules without pulling in game or editor layers.

    k_modules[] is the single declaration of intent. Loading RT_SERVICE(app) causes
    the host to create a window and pump OS events. Loading RT_MODULE(render) causes
    the host to drive begin_frame / draw_frame / end_frame each tick.

    Loop:  RT_LOOP_RUN
    Flags: RT_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/app/app.h"
#include "engine/core/core.h"

#include "runtime_module/render/render_api.h"
#include "runtime/host/host.h"

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
runtime_ready( void )
{
    /* Both app and render are initialized and the window exists — safe to call any API. */
    render_api()->set_clear_color( 0.08f, 0.10f, 0.18f );
    printf( "[example_runtime] ready — press Escape to quit, R to reload modules\n" );
}

static void
runtime_update( f32 dt )
{
    UNUSED( dt );

    if ( app_api()->key_pressed( APP_KEY_ESCAPE ) )
    {
        rt_host_quit();
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

static const rt_module_entry_t k_modules[] = {
    RT_SERVICE( app    ),   /* windowing, input — always statically linked */
    RT_SERVICE( core ),     /* logging, time, file I/O — always statically linked */   
    RT_MODULE ( render ),   /* renderer — hot-reloadable DLL in dynamic builds */
    { 0 },
};

static const rt_host_desc_t k_desc = {
    .name      = "example_runtime",
    .flags     = RT_HOST_HOT_RELOAD,
    .loop_mode = RT_LOOP_RUN,
    .modules   = k_modules,
    .on_ready  = runtime_ready,
    .on_update = runtime_update,
};

int
main( int argc, char** argv )
{
    return rt_host_main( &k_desc, argc, argv );
}

/*============================================================================================*/
