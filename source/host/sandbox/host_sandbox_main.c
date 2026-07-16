/*==============================================================================================

    sandbox_main.c : Sandbox launcher

    Instead of making a new .exe for each test project the sandbox launcher is a
    single .exe that can drive any module we point it at.

    It has no built-in assumptions about what modules it will run, but it does have
    hot-reload and console input handling built in.

    Boots through run_host_main headless (RUN_LOOP_NONE): the engine floor loads, then
    the caller (this file) would drive the requested sandbox module and shut down --
    module-loading itself is not implemented yet (see TODO below).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"

#include "host/common/host_common.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"

#include "runtime/run_host.h"

/*============================================================================================*/

static const run_module_entry_t k_modules[] = { { 0 } };

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    // launch_params_t params;
    // host_args_parse( argc, argv, &params );

    const run_host_desc_t desc = {
        .name      = "host_sandbox",
        .flags     = 0,               /* headless -- no window */
        .loop_mode = RUN_LOOP_NONE,   /* boot, return, caller drives, then shuts down */
        .modules   = k_modules,
    };

    if ( run_host_main( &desc, argc, argv ) != 0 )
        return 1;

    /* TODO: load and drive sandbox module specified by params.module_override */

    run_host_shutdown();
    return 0;
}

/*============================================================================================*/
