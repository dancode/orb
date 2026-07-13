#ifndef ASSET_H
#define ASSET_H
/*==============================================================================================

    runtime_service/asset/asset.h -- Asset service, public types.

    The asset service sits ABOVE core.  It turns a virtual path into a refcounted, typed,
    reloadable resource, deduplicating so many acquirers of the same path share one record.

    Layering: the fs library supplies BYTES by vpath; the asset service adds identity (asset_id_t),
    lifetime (refcount + state), and TYPE (an extension -> loader dispatch that decodes those
    bytes into a backend resource).  Because it lives above core it calls rhi/draw directly --
    there is no callback-into-core seam and core never holds a resource it cannot understand.

    Pure types only -- no function declarations, no vtable.  Callers include asset_api.h
    (DLL modules) or asset_host.h (host exes and sandboxes).

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Limits
==============================================================================================*/

#define ASSET_MAX          1024    // max concurrent asset records
#define ASSET_TYPE_MAX     32      // max registered asset types (index 0 = reserved "none")
#define ASSET_TYPE_EXTS    12      // max file extensions mapped to one type
#define ASSET_EXT_LEN      12      // bytes per extension incl. leading '.' and NUL
#define ASSET_TYPE_NAME    32      // bytes per type name incl. NUL

/*==============================================================================================
    Types
==============================================================================================*/

/* Stable, stale-safe handle to an asset record.  { index, generation }: index is 1-based
   (0 = invalid), generation is bumped when a slot is recycled so an old copy of the handle
   is detected as stale.  Pass by value. */
typedef struct asset_id_s
{
    u32 index;
    u32 generation;

} asset_id_t;

/* Load lifecycle.  LOADING is reserved so a future background/streaming loader slots in with
   no API change (Phase 2 loads synchronously: acquire goes UNLOADED -> LOADED/FAILED). */
typedef enum asset_state_e
{
    ASSET_UNLOADED = 0,
    ASSET_LOADING,
    ASSET_LOADED,
    ASSET_FAILED,

} asset_state_t;

/* Decode raw file bytes into a typed backend resource.  Return NULL on failure.
     vpath    -- the virtual path (for logging / relative lookups)
     data,size-- file bytes (NUL-terminated for convenience; size excludes the NUL)
     userdata -- the value passed to type_register for this type
   The bytes are owned by the caller (freed after load returns); copy what you need. */
typedef void* ( *asset_load_fn )( const char* vpath, const void* data, u32 size, void* userdata );

/* Release a resource previously produced by the matching load fn. */
typedef void  ( *asset_unload_fn )( void* resource, void* userdata );

/*============================================================================================*/
#endif    // ASSET_H
