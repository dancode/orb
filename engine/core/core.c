/*==============================================================================================

    core.c

==============================================================================================*/
#include "core/core.h"

// allow local engine debug natvis to find function values.

core_api_t*       g_api       = NULL;
core_debug_api_t* g_debug_api = NULL;

void
core_init( void )
{
    cvar_system_init();

    g_api       = core_get_api();
    g_debug_api = core_debug_get_api();
}

/*============================================================================================*/