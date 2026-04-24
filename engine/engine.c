/*==============================================================================================

    engine.c : (loader, api registry, simple memory/log implementation, main)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "orb.h"
#include "core.h"

#include "base/library.h"
#include "base/library.c"
#include "base/standard.c"


// #include "runtime/runtime.c"

/*==============================================================================================

    engine_module.c

    Registers engine_api as a static module so any DLL can retrieve it by name:

        engine_api_t* engine = sys->get_api("engine");

    engine depends on core being initialized first — we declare that here so
    the topo-sort always places engine after core.

==============================================================================================*/

#include "engine_api.h"
#include "core/module/module_api.h"
#include "core/module/module_sys.h"

static module_api_t g_engine_module_api = {
    .version    = 1,
    .state_size = 0,
    .deps       = { "core" },
    .dep_count  = 1,

    .init       = NULL,
    .tick       = NULL,
    .exit       = NULL,
    .on_reload  = NULL,
};

void
engine_module_register( void )
{
    module_register_static( "engine", &g_engine_module_api, engine_get_api() );
}

/*============================================================================================*/
