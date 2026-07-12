/*==============================================================================================

    sb_ref_dll.c -- Unity build entry for the sb_ref_dll module.

    The module ships no reflection plumbing of its own. Generated code
    (sb_ref_dll.generated.c) defines sb_ref_dll_ref_register(); the function
    pointer is passed to the mod system via the descriptor's ref_register slot, and the
    host's load callback invokes it. Same path for static and dynamic builds.

==============================================================================================*/

#include "orb.h"
#include <string.h>

#include "engine/mod/mod_export.h"
#include "engine/ref/ref_api.h"
#include "sandbox/reflect/sb_ref_dll/sb_ref_dll_api.h"
#include "sb_ref_dll.generated.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "runtime_modules/sb_ref_dll/sb_ref_dll_function.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef SB_REF_DLL_API_C_PRELUDE
#include "sandbox/reflect/sb_ref_dll/sb_ref_dll_api.c"
#endif

/*============================================================================================*/
