/*==============================================================================================

    example_reflect.c -- Unity build entry for the example_reflect module.

    The module ships no reflection plumbing of its own. Generated code
    (example_reflect.generated.c) defines example_reflect_ref_register(); the function
    pointer is passed to the mod system via the descriptor's ref_register slot, and the
    host's load callback invokes it. Same path for static and dynamic builds.

==============================================================================================*/

#include "orb.h"
#include <string.h>

#include "engine/mod/mod_export.h"
#include "engine/ref/ref_api.h"
#include "runtime_modules/example_reflect/example_reflect_api.h"
#include "example_reflect.generated.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "runtime_modules/example_reflect/example_reflect_function.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef EXAMPLE_REFLECT_API_C_PRELUDE
#include "runtime_modules/example_reflect/example_reflect_api.c"
#endif

/*============================================================================================*/
