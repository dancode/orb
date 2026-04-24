/*==============================================================================================

    core_module.c

    Registers core_api as a static module so any DLL can retrieve it by name:

        core_api_t* core = sys->get_api("core");

    core_api_t already lives in the exe's data segment (see core_api.c).
    We only need a thin module_api_t wrapper so the module system can manage it
    uniformly alongside dynamic modules.

==============================================================================================*/

#include "core/core_api.h"
#include "module_api.h"
#include "module_sys.h"

/* Forward: called by the module system with (state, core, engine).
   Core has no dependencies and no persistent state, so these are no-ops. */

static bool
core_mod_init( void* state, module_sys_api_t* sys )
{
    /* Core has no deps and no state — nothing to do. */
    ( void )state;
    ( void )sys;
    return true;
}

static module_api_t g_core_module_api = {
    .version    = 1,
    .state_size = 0,
    .dep_count  = 0,

    .init       = core_mod_init,
    .tick       = NULL,
    .exit       = NULL,
    .on_reload  = NULL,
};

void
core_module_register( void )
{
    /* exported_api is the live core_api_t* — what callers actually want. */
    module_register_static( "core", &g_core_module_api, core_get_api() );
}
