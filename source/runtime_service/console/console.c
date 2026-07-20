/*==============================================================================================

    runtime_service/console/console.c -- Unity build entry for the developer console service.

    Includes order: console_api.h first (types + the console() gateway), then the sibling API
    headers (core = the console/cmd backend, app = the toggle keys, gui = the front end), then
    the view implementation, then console_api.c last so the vtable initializer can see every
    static function.  The console service is a STATIC service (like rhi/draw/gui/input): linked
    into the host and registered via RUN_SERVICE, not a hot-reloaded DLL.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "console_api.h"
#include "engine/core/core_api.h"
#include "engine/app/app_api.h"
#include "runtime_service/gui/gui_api.h"

/* File-scope cached API pointers.  core() = the con_* backend (scrollback, history, submit,
   completion); app() = the grave/escape toggle keys; gui() = the drop-down front end.
   Static builds: no-op. */
MOD_USE_CORE;
MOD_USE_APP;
MOD_USE_GUI;

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/console/console_view.c"

#ifndef CONSOLE_API_C_PRELUDE
    #include "runtime_service/console/console_api.c"
#endif

/*============================================================================================*/
