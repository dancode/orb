/*==============================================================================================

    core.c

==============================================================================================*/
#include "core/core.h"

// allow local engine debug natvis to find function values.

// core_api_t*       g_api       = NULL;
// core_debug_api_t* g_debug_api = NULL;

/*============================================================================================*/
// export internal data for debug api for NatVis

void
core_init( void )
{
    core_api_init();
    cvar_system_init();
}

/*============================================================================================*/