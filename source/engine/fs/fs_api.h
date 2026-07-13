#ifndef FS_API_H
#define FS_API_H
/*==============================================================================================

    engine/fs/fs_api.h -- fs module API struct and fs() gateway macro.
    fs is always statically linked into the host.

==============================================================================================*/

#include "engine/fs/fs.h"
#include "engine/mod/mod_import.h"

/*==============================================================================================
    API Struct

    Vtable names drop the fs_ prefix -- the gateway supplies the subject: fs()->read( vpath ).
==============================================================================================*/

typedef struct fs_api_s
{
    /* Map a virtual prefix onto a real directory (or a .zip archive).  `vprefix` may be ""
       to match every vpath.  `priority` breaks collisions -- the highest-priority mount that
       actually has the file wins.  Returns false if the mount table is full. */
    bool ( *mount )( const char* vprefix, const char* real_path, int priority );

    /* Remove the mount whose (normalized) virtual prefix matches `vprefix`.  Drops the catalog
       cache (it rebuilds lazily on the next read). */
    void ( *unmount )( const char* vprefix );

    /* Read an entire file resolved through the mount table.  See fs_blob_t. */
    fs_blob_t ( *read )( const char* vpath );

    /* Release a blob returned by read (safe on a failed read). */
    void ( *free )( fs_blob_t* blob );

    /* True if the vpath resolves to a file. */
    bool ( *exists )( const char* vpath );

    /* Size + mtime without reading bytes. */
    bool ( *stat )( const char* vpath, fs_stat_t* out );

    /* "dir/pattern" match over DIR mounts only; ZIP entries are not enumerated (reads still
       work).  Returns the number of matches visited. */
    int ( *glob )( const char* vpat, fs_glob_fn cb, void* userdata );

    /* Number of files currently in the catalog (introspection / tests). */
    u32 ( *file_count )( void );

} fs_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( FS_STATIC )
    MOD_GATEWAY_STATIC( fs_api_t, fs )
    #define MOD_USE_FS    /* static build */
    #define MOD_FETCH_FS  true
#else
    MOD_GATEWAY_DYNAMIC( fs_api_t, fs )
    #define MOD_USE_FS    MOD_DEFINE_API_PTR( fs_api_t, fs )
    #define MOD_FETCH_FS  MOD_FETCH_API( fs_api_t, fs )
#endif

/*============================================================================================*/
#endif    // FS_API_H
