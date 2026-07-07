/*==============================================================================================

    engine/core/core_fs.c -- unity glue for the virtual filesystem subsystem.

    Pulls fs into the core translation unit (like core_cvar.c does for cvar/cmd/console).
    core_init/core_exit drive fs_system_init/exit; the fs_* functions are exposed both
    directly (host/sandbox linkers) and through the core_api_t vtable (DLL consumers).

==============================================================================================*/

#include "engine/core/fs/fs.h"
#include "engine/core/fs/fs.c"

/*============================================================================================*/
