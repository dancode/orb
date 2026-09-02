#ifndef ASSET_H
#define ASSET_H
/*==============================================================================================

    runtime_service/asset/asset.h -- Asset service, public types.

    The asset service sits ABOVE core.  It turns a resource name into a refcounted, typed,
    reloadable resource, deduplicating so many acquirers of one name share one record.

    Layering: a name is a path under a content root minus the extension (engine/res/res.h);
    fs supplies BYTES by path; the asset service adds the identity of the loaded INSTANCE
    (aid_t), lifetime (refcount + state), and TYPE.  The caller names the type it wants when it
    acquires, and the type's loader owns the file extensions it accepts, in preference order:
    the service asks fs for the name plus each extension in turn -- ui/icon/save.tex, then
    ui/icon/save.png -- and the mount order decides which tree answers (loose content, a
    cooked mirror, a shipped pack).  Because it lives above core it calls rhi/draw directly --
    there is no callback-into-core seam and core never holds a resource it cannot understand.

    Pure types only -- no function declarations, no vtable.  Callers include asset_api.h
    (DLL modules) or asset_host.h (host exes and sandboxes).

==============================================================================================*/

#include "orb.h"
#include "engine/res/res.h"

/*==============================================================================================
    Limits
==============================================================================================*/

#define ASSET_MAX          1024    // max concurrent asset records
#define ASSET_TYPE_MAX     32      // max registered asset types (index 0 = reserved "none")
#define ASSET_TYPE_EXTS    12      // max file extensions accepted by one type
#define ASSET_EXT_LEN      12      // bytes per extension incl. leading '.' and NUL
#define ASSET_TYPE_NAME    32      // bytes per type name incl. NUL
#define ASSET_PATH_MAX     ( RES_NAME_MAX + ASSET_EXT_LEN )   // a resolved vpath: name + ext + NUL

/*==============================================================================================
    Built-in types

    Registered by the service at init, in this order, so their ids are fixed and a caller can
    name them without a lookup.  Custom types use the id type_register returns.
==============================================================================================*/

#define ASSET_TYPE_NONE    0
#define ASSET_TYPE_IMAGE   1       // loaders/asset_image.h  -- get() returns asset_image_t*
#define ASSET_TYPE_SHADER  2       // loaders/asset_shader.h -- get() returns asset_shader_t*

/*==============================================================================================
    Types
==============================================================================================*/

/* Stable, stale-safe handle to a loaded instance.  { index, generation }: index is 1-based
   (0 = invalid), generation is bumped when a slot is recycled so an old copy of the handle is
   detected as stale.  Pass by value.  An aid_t exists only while something holds the instance;
   the name it was loaded from is a fact about content and outlives it. */
typedef struct aid_s
{
    u32 index;
    u32 generation;

} aid_t;

/* Load lifecycle.  LOADING is reserved so a future background/streaming loader slots in with
   no API change (loads are synchronous today: acquire goes UNLOADED -> LOADED/FAILED). */
typedef enum asset_state_e
{
    ASSET_UNLOADED = 0,
    ASSET_LOADING,
    ASSET_LOADED,
    ASSET_FAILED,

} asset_state_t;

/* Decode raw file bytes into a typed backend resource.  Return NULL on failure.
     path     -- the vpath that was read: the resource name plus the extension that matched
                 (for logging / debug names)
     data,size-- file bytes (NUL-terminated for convenience; size excludes the NUL)
     userdata -- the value passed to type_register for this type
   The bytes are owned by the caller (freed after load returns); copy what you need. */
typedef void* ( *asset_load_fn )( const char* path, const void* data, u32 size, void* userdata );

/* Release a resource previously produced by the matching load fn. */
typedef void  ( *asset_unload_fn )( void* resource, void* userdata );

/*============================================================================================*/
#endif    // ASSET_H
