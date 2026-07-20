#ifndef CONSOLE_H
#define CONSOLE_H
/*==============================================================================================

    runtime_service/console/console.h -- Developer console UI service, public types.

    The console DATA (scrollback, command registry, history, dispatch) already lives in core
    (core/cmd + core/console) and works headless over stdout.  THIS service is the gui FRONT
    END: a Quake-style drop-down that renders core()->con_line_get() and feeds keystrokes back
    through core()->con_submit().  It sits above core and above gui, so it cannot live in core.

    Layering: the HOST drives it at two points (guarded if ( console() ), so it stays optional):
        console()->frame( dt )       -- once per frame, near input()->frame(): grave/escape
                                        toggle + the post-submit redraw pin.  No widgets.
        console()->emit( dt, vp )    -- inside the gui frame build (after on_gui, before
                                        ctx_end): draws the drop-down when open.

    Pure types only -- no function declarations, no vtable.  Callers include console_api.h
    (DLL modules) or console_host.h (host exes and sandboxes).

==============================================================================================*/

#include "orb.h"
#include "runtime_service/gui/gui.h"    /* gui_vp_t for the emit() viewport argument */

/*==============================================================================================
    Limits
==============================================================================================*/

#define CONSOLE_ROWS      18     // visible scrollback rows in the drop-down
#define CONSOLE_INPUT_MAX 120    // input line capacity (fits CON_HISTORY_LEN)

/*============================================================================================*/
#endif    // CONSOLE_H
