/*==============================================================================================

    sandbox_tool_main.c — TOOL shape.

    Boots the engine, runs a single frame, exits.
    All tool work happens in on_update. Load additional modules in k_modules as needed
    (e.g. a shader compiler module, an asset importer, etc.).

    Loop:  RT_LOOP_ONCE
    Flags: RT_HOST_CONSOLE

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "runtime/host/host.h"

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

static const rt_module_entry_t k_modules[] = {
    /* add tool-specific modules here, e.g.:
       RT_SERVICE( asset_compiler ), */
    { 0 } };

static const rt_host_desc_t k_desc = {
    .name      = "sandbox_tool",
    .flags     = RT_HOST_CONSOLE,
    .loop_mode = RT_LOOP_ONCE,
    .modules   = k_modules,
    .on_update = tool_update,
};

/*============================================================================================*/

int
main( int argc, char** argv )
{
    return rt_host_main( &k_desc, argc, argv );
}

/*============================================================================================*/