/*==============================================================================================

    core.c

    This is the main compilation unit for the core module. It compiles the core API and
    initializes the core systems that other modules depend on, such as the string interning
    system (sid) and cvar system.

==============================================================================================*/

#include "core/core.h"
#include "core/core_api.h"

/*============================================================================================*/

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