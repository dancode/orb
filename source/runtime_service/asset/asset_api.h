#ifndef ASSET_API_H
#define ASSET_API_H
/*==============================================================================================

    runtime_service/asset/asset_api.h -- Asset service API struct and gateway macro.

    Include this in DLL .c files that call the asset service through the vtable.  Host
    executables and sandboxes include asset_host.h instead.

    Function groups (all called through the asset() vtable):
        Types  : type_register
        Assets : acquire / release / reload / refresh
        Query  : get / state / valid / refcount / name / count

==============================================================================================*/

#include "runtime_service/asset/asset.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct asset_api_s
{
    /* Register an asset type: a name, the file extensions it accepts IN PREFERENCE ORDER (each
       like ".tex"; a cooked form first, the source forms it can decode after), and its
       load/unload pair.  `userdata` is passed back to both callbacks.  Returns the type id
       (>=1), or 0 if the type table is full.  Game and editor DLLs use this to add their own
       asset kinds -- dispatch stays inside the service, it is not a core seam. */
    u16 ( *type_register )( const char* name, const char* const* exts, u32 ext_count,
                            asset_load_fn load, asset_unload_fn unload, void* userdata );

    /* Find-or-create the record for `name` loaded as `type` and increment its refcount.  The
       name is a path under a content root minus the extension, canonical lowercase (res.h);
       mark it with RID() at the call site so the build resolves it and lists it in the
       image's resource manifest.  A name that is not canonical (res_name_ok) is refused with
       the invalid handle.  The first acquire loads synchronously: the type's extensions are
       tried in order against fs, and the first that exists is read and handed to the type's
       load fn.  On a load failure (no file under any extension, or the loader rejected the
       bytes) the handle is valid but state()==ASSET_FAILED and get()==NULL; release it like
       any other.  Acquiring a name already held as a DIFFERENT type is an error and returns
       the invalid handle: one resource, one type. */
    aid_t ( *acquire )( const char* name, u16 type );

    /* Decrement the refcount.  At zero the resource is unloaded, the record is freed, and the
       handle's slot is recycled (its generation bumps, invalidating stale copies). */
    void ( *release )( aid_t id );

    /* Re-run the loader for an id in place (hot-reload hook): unloads the current resource and
       loads fresh, re-probing the type's extensions, keeping the same id and refcount. */
    void ( *reload )( aid_t id );

    /* Poll every live asset for a changed source file and reload the ones that changed (mtime
       compare on the file that loaded; also retries any that are FAILED).  Ids and refcounts
       are preserved, so callers who re-get() see the fresh resource.  Returns how many were
       reloaded.  Call at a modest cadence (a few times a second) -- it stats each live
       source.  A higher-preference extension appearing beside a loaded file (a .tex cooked
       next to the .png that loaded) is not noticed until that record reloads. */
    u32 ( *refresh )( void );

    /* The typed resource pointer if the asset is LOADED, else NULL. */
    void* ( *get )( aid_t id );

    /* Current asset_state_t (as int), or ASSET_UNLOADED for an invalid/stale id. */
    int ( *state )( aid_t id );

    /* True if the handle names a live record with a matching generation. */
    bool ( *valid )( aid_t id );

    /* Current refcount, or 0 for an invalid/stale id. */
    i32 ( *refcount )( aid_t id );

    /* The name this instance was acquired under (interned; valid for the life of the program),
       or NULL for an invalid/stale id. */
    const char* ( *name )( aid_t id );

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
