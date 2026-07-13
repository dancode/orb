#ifndef FS_HOST_H
#define FS_HOST_H
/*==============================================================================================

    engine/fs/fs_host.h -- host-only fs API: lifecycle, direct calls, and the module descriptor.
    Includes fs_api.h.

    The three-header split keeps the dependency direction honest: a DLL can only see the vtable
    surface (fs_api.h), so it physically cannot link host internals; the host and sandboxes get
    these direct declarations because fs is statically linked into them.  The same functions
    appear twice on purpose -- once as pointers in fs_api_t, once as direct decls here -- and
    fs_api.c is where both surfaces are proven to be the same code.

==============================================================================================*/

#include "engine/fs/fs_api.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

/*==============================================================================================
    Lifecycle

    fs_mod_init / fs_mod_exit (in fs_api.c) drive these when fs is loaded as a module; a
    direct host/test that does not run the module system may call them itself.  Idempotent
    enough for a clean re-init; must only run on the main thread.
==============================================================================================*/

void        fs_system_init  ( void );
void        fs_system_exit  ( void );

/*==============================================================================================
    Direct-call functions (host and sandbox use only)

    Twins of the fs_api_t vtable -- see fs_api.h for the contracts.
==============================================================================================*/

bool        fs_mount        ( const char* vprefix, const char* real_path, int priority );
void        fs_unmount      ( const char* vprefix );
fs_blob_t   fs_read         ( const char* vpath );
void        fs_free         ( fs_blob_t* blob );
bool        fs_exists       ( const char* vpath );
bool        fs_stat         ( const char* vpath, fs_stat_t* out );
int         fs_glob         ( const char* vpat, fs_glob_fn cb, void* userdata );
u32         fs_file_count   ( void );

/*==============================================================================================
    Module Descriptor

    Used by the host to register the fs module:
        mod_static_load( "fs", fs_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_static( fs );
==============================================================================================*/

mod_desc_t* fs_get_mod_desc ( void );

/*============================================================================================*/
#endif    // FS_HOST_H
