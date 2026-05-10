/*==============================================================================================

    runtime/host/runtime_host.c : The shared platform-agnostic "runtime host" module.

    This is the minimal scaffolding layer for runtime applications. It provides the
    main loop and module management, but doesn't contain any game-specific logic.

    The expectation is that the project DLL (e.g. amberfall.dll) will contain the
    game-specific code, and will be loaded as a module by this runtime host.

    This module serves as the universal entry point for all non-tool engine applications.
    It is responsible for initializing the module system, loading the necessary modules
    (both static and dynamic), and running the main loop until an exit condition is met.

    TLDL: So we don't have to repeat this boilerplate for every project/sandbox we create.

==============================================================================================*/
#ifndef RUNTIME_HOST_H
#define RUNTIME_HOST_H

#include "orb.h"

// The configuration passed from the executable (parsed by host_common)

typedef struct
{
    const char* project_name;    // e.g., "Amberfall" (used for Window Title)
    const char* project_dll;     // e.g., "amberfall" (used for module loading)

    uint32_t    window_width;     // Initial window width (ignored if headless)
    uint32_t    window_height;    // Initial window height (ignored if headless)

    bool        is_headless;    // If true, bypasses the App/Window module entirely

} runtime_config_t;

// The universal entry point for all non-tool engine applications
int runtime_host_run( const runtime_config_t* config );

/*============================================================================================*/
#endif    // RUNTIME_HOST_H