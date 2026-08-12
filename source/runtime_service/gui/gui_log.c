/*==============================================================================================

    runtime_service/gui/gui_log.c -- GUI_LOG translation unit: the diagnostics floor.

    A small message pipe the rest of the GUI uses to report warnings and errors -- "this
    pool is full," "this asset failed to load," and the like. Call gui_log() and the message
    goes to whichever sink is currently installed: a default one that just prints to the
    console, or a host-supplied one that can route it anywhere (a log file, an in-app console
    window). It formats the message and hands it off; it never decides what happens to it.

    This is the lowest-level unit in the entire GUI -- even GUI_RECT can report through it if a
    fixed-size pool runs out -- so it depends on nothing but base types and the C runtime, and
    every other unit can reach it through gui.h.

    Constituents (log/):
        gui_log_core.c   -- sink storage, message formatting, default printf dispatch

==============================================================================================*/

#include <stdio.h>     /* printf / fflush -- the default sink                     */
#include <stdarg.h>    /* va_list -- gui_log is variadic                          */

#include "orb.h"
#include "base/fmt.h"  /* fmt_vsnprintf -- locale-free bounded formatting         */

#include "runtime_service/gui/log/gui_log.h"
#include "runtime_service/gui/log/gui_log_core.c"

/*============================================================================================*/
