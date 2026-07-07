#ifndef INPUT_HOST_H
#define INPUT_HOST_H
/*==============================================================================================

    runtime_service/input/input_host.h -- Host-only input interface.  Includes input_api.h.

    Include this in host executables, unity build entries, and test sandboxes.
    DLL modules that only call the input service through the vtable include input_api.h.

    How to register it in a sandbox or host:

        #include "runtime_service/input/input_host.h"

        mod_static( input );   // or: mod_static_load( "input", input_get_mod_desc() )

    How a DLL module calls it:

        #include "runtime_service/input/input_api.h"

        MOD_USE_INPUT   // file scope

        // in init()/reload():
        if (!MOD_FETCH_INPUT) return false;
        input_action_t fwd = input()->action_register( "forward", INPUT_ACTION_BUTTON, CTX_GAME );

        // per frame:
        if ( input()->down( fwd ) ) ...

    The deps "core"/"app" in the mod_desc_t ensure the cmd registry and the device layer
    are initialized first.  The HOST LOOP must call input()->frame( dt ) once per frame,
    AFTER cmd_pump(), so bind edges resolve into the same frame's pressed/released counts
    (run_host does this automatically when the service is loaded).

==============================================================================================*/

#include "runtime_service/input/input_api.h"
#include "engine/mod/mod_export.h"

/* Module descriptor -- pass to mod_static_load() to register the input service. */
mod_desc_t* input_get_mod_desc( void );

/*============================================================================================*/
#endif    // INPUT_HOST_H
