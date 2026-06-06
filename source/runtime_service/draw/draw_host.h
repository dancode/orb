#ifndef DRAW_HOST_H
#define DRAW_HOST_H
/*==============================================================================================

    runtime_service/draw/draw_host.h -- Host-only draw interface.  Includes draw_api.h.

    Include this in host executables, unity build entries, and test sandboxes.
    DLL modules that only call draw through the vtable include draw_api.h instead.

    How to register it in a sandbox or host:

        #include "runtime_service/draw/draw_host.h"

        mod_static( draw );   // or: mod_static_load( "draw", draw_get_mod_desc() )

    How a DLL module calls draw:

        #include "runtime_service/draw/draw_api.h"

        MOD_USE_DRAW   // file scope

        // in init()/reload():
        if (!MOD_FETCH_DRAW) return false;

        // call site:
        draw()->rect( 0.f, 0.f, 0.5f, 0.3f, red );

    The dep "rhi" in the mod_desc_t means the module system ensures rhi is initialized
    before draw, so get_api("rhi") is guaranteed to succeed in draw_mod_init.

==============================================================================================*/

#include "runtime_service/draw/draw_api.h"
#include "engine/mod/mod_export.h"

/* Module descriptor -- pass to mod_static_load() to register draw with the mod system. */
mod_desc_t* draw_get_mod_desc( void );

/*============================================================================================*/
#endif    // DRAW_HOST_H
