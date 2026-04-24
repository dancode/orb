/*==============================================================================================

    core.c

==============================================================================================*/

#include "core/core.h"
#include "core/core_api.h"
#include "core/module.h"

void
core_init( void )
{
    core_api_init();
    sid_init();
    cvar_system_init();
}

void
core_exit( void )
{
    cvar_system_exit();
    sid_exit();
    core_api_exit();
}

/*============================================================================================*/