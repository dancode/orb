/*==============================================================================================

    physics.c -- Unity build entry for the physics module.

==============================================================================================*/

#include "orb.h"
#define LOG_CH "physics"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"
#include "runtime_modules/physics/physics_api.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "runtime_modules/physics/physics_function.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef PHYSICS_API_C_PRELUDE
#include "runtime_modules/physics/physics_api.c"
#endif

/*============================================================================================*/
