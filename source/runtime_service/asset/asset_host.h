#ifndef ASSET_HOST_H
#define ASSET_HOST_H
/*==============================================================================================

    runtime_service/asset/asset_host.h -- Host-only asset interface.  Includes asset_api.h.

    Include this in host executables, unity build entries, and test sandboxes.
    DLL modules that only call the asset service through the vtable include asset_api.h.

    How to register it in a sandbox or host:

        #include "runtime_service/asset/asset_host.h"

        mod_static( asset );   // or: mod_static_load( "asset", asset_get_mod_desc() )

    How a DLL module calls it:

        #include "runtime_service/asset/asset_api.h"

        MOD_USE_ASSET   // file scope

        // in init()/reload():
        if (!MOD_FETCH_ASSET) return false;

        // call site:
        asset_id_t id = asset()->acquire( "textures/hero.png" );

    The dep "core" in the mod_desc_t ensures core (and its virtual filesystem) is initialized
    before the asset service, so core()->fs_read succeeds inside the loaders.  Mount the
    directories the assets live in (core()->fs_mount) before acquiring.

==============================================================================================*/

#include "runtime_service/asset/asset_api.h"
#include "engine/mod/mod_export.h"

/* Module descriptor -- pass to mod_static_load() to register the asset service. */
mod_desc_t* asset_get_mod_desc( void );

/*============================================================================================*/
#endif    // ASSET_HOST_H
