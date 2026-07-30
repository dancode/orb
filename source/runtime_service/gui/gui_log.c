/*==============================================================================================

    runtime_service/gui/gui_log.c -- GUI_LOG translation unit: the diagnostics floor.

    The bottom unit of the whole stack -- below GUI_RECT, since a rect pool that saturates
    reports through this.  It depends on nothing but base types and the CRT, and every other
    unit reaches its one header through gui.h.

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
