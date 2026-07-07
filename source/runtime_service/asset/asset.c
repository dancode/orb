/*==============================================================================================

    runtime_service/asset/asset.c -- Unity build entry for the asset service.

    Includes order: core_api.h first (types + the core() gateway used throughout), then the
    registry implementation, then asset_api.c last so the vtable initializer can see every
    static function.  The asset service is a STATIC service (like rhi/draw/gui): linked into
    the host and registered via mod_static, not a hot-reloaded DLL.

==============================================================================================*/

#include "asset_api.h"
#include "engine/core/core_api.h"

/* File-scope cached core API pointer used by the registry (fs_read / sid / alloc).
   Static builds: no-op (shared global struct used directly). */
MOD_USE_CORE;

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/asset/asset_registry.c"

#ifndef ASSET_API_C_PRELUDE
    #include "runtime_service/asset/asset_api.c"
#endif

/*============================================================================================*/
