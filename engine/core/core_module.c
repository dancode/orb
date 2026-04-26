/*==============================================================================================

    core_module.c

    Registers core_api as a static module so any DLL can retrieve it by name:

        core_api_t* core = sys->get_api("core");

    core_api_t already lives in the exe's data segment (see core_api.c).
    We only need a thin module_api_t wrapper so the module system can manage it
    uniformly alongside dynamic modules.

==============================================================================================*/

#include "core/core.h"
#include "core/core_api.h"
#include "module/module_api.h"

/*============================================================================================*/

static bool
core_mod_init( void* state, module_sys_api_t* sys )
{
    /* Core has no deps and no state — nothing to do. */
    UNUSED( state );
    UNUSED( sys );

    return true;
}

static bool
core_mod_exit( void* state )
{
    UNUSED( state );
    return true;
}

/* core_module.c */
module_api_t*
core_get_module_api( void )
{
    static module_api_t module_api = {
        .version    = 1,
        .state_size = 0,
        .dep_count  = 0,

        .init       = core_mod_init,
        .tick       = NULL,
        .exit       = core_mod_exit,
        .on_reload  = NULL,
    };

    return &module_api;
}

/*============================================================================================*/
