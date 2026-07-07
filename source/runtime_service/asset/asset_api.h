#ifndef ASSET_API_H
#define ASSET_API_H
/*==============================================================================================

    runtime_service/asset/asset_api.h -- Asset service API struct and gateway macro.

    Include this in DLL .c files that call the asset service through the vtable.  Host
    executables and sandboxes include asset_host.h instead.

    Function groups (all called through the asset() vtable):
        Types  : type_register
        Assets : acquire / release / reload / refresh
        Query  : get / state / valid / refcount / count

==============================================================================================*/

#include "runtime_service/asset/asset.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct asset_api_s
{
    /* Register an asset type: a name plus the file extensions it claims (each like ".png",
       matched case-insensitively) and its load/unload pair.  `userdata` is passed back to
       both callbacks.  Returns the type id (>=1), or 0 if the type table is full.  Game and
       editor DLLs use this to add their own asset kinds -- dispatch stays inside the service,
       it is not a core seam. */
    u16 ( *type_register )( const char* name, const char* const* exts, u32 ext_count,
                            asset_load_fn load, asset_unload_fn unload, void* userdata );

    /* Find-or-create the record for `vpath` and increment its refcount.  Paths are matched
       after normalization (backslashes folded, case-insensitive), so "a/B.png", "a\\b.png"
       resolve to the SAME record -- that is the dedup.  The first acquire loads synchronously
       (fs_read -> the type's load fn).  Always returns a handle you must release; on a load
       failure the handle is valid but state()==ASSET_FAILED and get()==NULL. */
    asset_id_t ( *acquire )( const char* vpath );

    /* Decrement the refcount.  At zero the resource is unloaded, the record is freed, and the
       handle's slot is recycled (its generation bumps, invalidating stale copies). */
    void ( *release )( asset_id_t id );

    /* Re-run the loader for an id in place (hot-reload hook): unloads the current resource and
       loads fresh from disk, keeping the same id and refcount. */
    void ( *reload )( asset_id_t id );

    /* Poll every live asset for a changed source file and reload the ones that changed (mtime
       compare; also retries any that are FAILED).  Ids and refcounts are preserved, so callers
       who re-get() see the fresh resource.  Returns how many were reloaded.  Call at a modest
       cadence (a few times a second) -- it stats each live source. */
    u32 ( *refresh )( void );

    /* The typed resource pointer if the asset is LOADED, else NULL. */
    void* ( *get )( asset_id_t id );

    /* Current asset_state_t (as int), or ASSET_UNLOADED for an invalid/stale id. */
    int ( *state )( asset_id_t id );

    /* True if the handle names a live record with a matching generation. */
    bool ( *valid )( asset_id_t id );

    /* Current refcount, or 0 for an invalid/stale id. */
    i32 ( *refcount )( asset_id_t id );

    /* Number of live asset records. */
    u32 ( *count )( void );

} asset_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( ASSET_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
MOD_GATEWAY_STATIC( asset_api_t, asset )
    #define MOD_USE_ASSET     /* static: gateway returns pointer to global struct directly */
    #define MOD_FETCH_ASSET   true
#else
MOD_GATEWAY_DYNAMIC( asset_api_t, asset )
    #define MOD_USE_ASSET     MOD_DEFINE_API_PTR( asset_api_t, asset )
    #define MOD_FETCH_ASSET   MOD_FETCH_API( asset_api_t, asset )
#endif

// clang-format on
/*============================================================================================*/
#endif    // ASSET_API_H
