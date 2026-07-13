/*==============================================================================================

    engine/fs/fs_api.c -- fs API struct and module descriptor.

    Included last by fs.c so every implementation function is visible in the TU.

==============================================================================================*/
#ifndef FS_API_C_PRELUDE
#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/fs/fs_host.h"
#endif

/* INFO: This file is what makes fs a "module" like any DLL: one descriptor (api struct, sizes,
   init/exit) registered with the module system by the host.  The registry copies
   &g_fs_api_struct into a stable slot; every module that says MOD_FETCH_FS receives that slot
   -- which is how a hot-reloaded game DLL finds the vfs living inside the host exe.
   func_api_size is the ABI handshake: if a reloaded DLL disagrees on the struct size, the swap
   is refused up front instead of corrupting memory through a mis-shaped table. */

/*==============================================================================================
    API Struct
==============================================================================================*/

const fs_api_t g_fs_api_struct =
{
    .mount      = fs_mount,
    .unmount    = fs_unmount,
    .read       = fs_read,
    .free       = fs_free,
    .exists     = fs_exists,
    .stat       = fs_stat,
    .glob       = fs_glob,
    .file_count = fs_file_count,
};

/*==============================================================================================
    Module Integration
==============================================================================================*/

static bool
fs_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    /* fs is a leaf module (deps: sys, statically linked alongside it in the host).  Bring up
       the mount table + catalog, then publish fs_api_t through the standard gateway for every
       module loaded after it. */
    fs_system_init();
    return true;
}

static void
fs_mod_exit( void* state )
{
    UNUSED( state );
    fs_system_exit();
}

mod_desc_t*
fs_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( fs_api_t ),
        .func_api      = ( void* )&g_fs_api_struct,
        .deps          = { "sys" },
        .dep_count     = 1,
        .init          = fs_mod_init,
        .exit          = fs_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/
