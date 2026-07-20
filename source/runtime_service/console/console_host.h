#ifndef CONSOLE_HOST_H
#define CONSOLE_HOST_H
/*==============================================================================================

    runtime_service/console/console_host.h -- Host-only console interface.  Includes console_api.h.

    Include this in host executables, unity build entries, and test sandboxes.
    DLL modules that only call the console service through the vtable include console_api.h.

    How to register it in a host:

        #include "runtime_service/console/console_host.h"

        RUN_SERVICE( console )   // in the host's k_modules[]

    The deps "core"/"app"/"gui" in the mod_desc_t ensure the console backend, the device
    layer, and the gui front end are all initialized first.  The HOST drives it each frame:
    console()->frame( dt ) near input()->frame(), and console()->emit( dt, vp ) inside the gui
    frame build (run_host does both automatically when the service is loaded).

==============================================================================================*/

#include "runtime_service/console/console_api.h"
#include "engine/mod/mod_export.h"

/* Module descriptor -- pass to RUN_SERVICE()/mod_static_load() to register the console. */
mod_desc_t* console_get_mod_desc( void );

/*============================================================================================*/
#endif    // CONSOLE_HOST_H
