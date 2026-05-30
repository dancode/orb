/*==============================================================================================

    sandbox_tool_main.c — TOOL shape.

    Boots the engine, runs a single frame, exits.
    All tool work happens in on_update. Load additional modules in k_modules as needed
    (e.g. a shader compiler module, an asset importer, etc.).

    Loop:  RUN_LOOP_ONCE
    Flags: RUN_HOST_CONSOLE

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "runtime/runtime_host.h"

/*==============================================================================================
    Tool logic — runs once
==============================================================================================*/

static void
tool_update( f32 dt )
{
    UNUSED( dt );
    printf( "[tool] running...\n" );
    /* drive tool modules here via their typed APIs */
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    /* add tool-specific modules here, e.g.:
       RUN_SERVICE( asset_compiler ), */
    { 0 } };

static const run_host_desc_t k_desc = {
    .name      = "sandbox_tool",
    .flags     = RUN_HOST_CONSOLE,
    .loop_mode = RUN_LOOP_ONCE,
    .modules   = k_modules,
    .on_update = tool_update,
};

/*============================================================================================*/

int
main( int argc, char** argv )
{
    return run_host_main( &k_desc, argc, argv );
}

/*============================================================================================*/