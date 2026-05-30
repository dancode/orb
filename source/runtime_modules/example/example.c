/*==============================================================================================

    example.c -- Unity build entry for the example module.

    Compiled as a DLL in dynamic builds; linked into the exe in static builds.
    The module system loads this at runtime and calls its lifecycle callbacks.
    It can also hot-reload it when the file changes on disk.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>

#include "engine/mod/mod_export.h"
#include "runtime_modules/example/example_api.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "runtime_modules/example/example_function.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef EXAMPLE_API_C_PRELUDE
#include "runtime_modules/example/example_api.c"
#endif

/*============================================================================================*/
