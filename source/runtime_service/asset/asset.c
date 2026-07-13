/*==============================================================================================

    runtime_service/asset/asset.c -- Unity build entry for the asset service.

    Includes order: core_api.h first (types + the core() gateway used throughout), then the
    registry implementation, then asset_api.c last so the vtable initializer can see every
    static function.  The asset service is a STATIC service (like rhi/draw/gui): linked into
    the host and registered via mod_static, not a hot-reloaded DLL.

==============================================================================================*/

#include "asset_api.h"
#include "engine/core/core_api.h"
#include "engine/fs/fs_api.h"
#include "runtime_service/rhi/rhi_api.h"

/* File-scope cached API pointers.  fs() = read bytes by vpath; core() = sid / alloc (registry);
   rhi() = texture create/upload/bindless (the image loader).  Static builds: no-op (shared
   globals used). */
MOD_USE_FS;
MOD_USE_CORE;
MOD_USE_RHI;

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/asset/asset_registry.c"
#include "runtime_service/asset/loaders/asset_image.c"
#include "runtime_service/asset/loaders/asset_shader.c"

#ifndef ASSET_API_C_PRELUDE
    #include "runtime_service/asset/asset_api.c"
#endif

/*============================================================================================*/
