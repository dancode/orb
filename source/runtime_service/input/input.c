/*==============================================================================================

    runtime_service/input/input.c -- Unity build entry for the input action service.

    Includes order: input_api.h first (types + the input() gateway), then the sibling API
    headers (core = cmd registry, app = devices), then the implementation, then input_api.c
    last so the vtable initializer can see every static function.  The input service is a
    STATIC service (like rhi/draw/gui/asset): linked into the host and registered via
    mod_static, not a hot-reloaded DLL.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "input_api.h"
#include "engine/core/core_api.h"
#include "engine/app/app_api.h"

/* File-scope cached API pointers.  core() = cmd_register / con_printf (the +/- transport);
   app() = pad axes + raw mouse (axis sources, next phase).  Static builds: no-op. */
MOD_USE_CORE;
MOD_USE_APP;

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/input/input_actions.c"

#ifndef INPUT_API_C_PRELUDE
    #include "runtime_service/input/input_api.c"
#endif

/*============================================================================================*/
