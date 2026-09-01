/*==============================================================================================

    engine/res/res.h - Resource catalogue types: rid_t, name hashing, resource tables.

    Pure types and inline hashing only -- no vtable, no function declarations.
    Include engine/res/res_api.h  for the runtime vtable (DLL modules).
    Include engine/res/res_host.h for direct function calls (hosts, sandboxes, tools).

    A rid_t names a resource that CAN be loaded: a font size, an icon, a shader, a level.
    It is dead data -- a dictionary entry, not a live instance (that is the asset service's
    aid_t).  The id is a pure function of a logical name, so any two sites that spell the
    same name hold the same id with no registry round-trip, and a hot-reloaded DLL re-finds
    the ids it declared.

    Names are logical and hierarchical, never file paths: "ui/icon/save" or
    "font/cascadia_mono/16".  No extension, no directory coupling.  The build resolves a
    name to a cooked file; source code never does.

==============================================================================================*/
#ifndef RES_H
#define RES_H

#include "orb.h"

// clang-format off
/*==============================================================================================
    rid_t

    32-bit FNV-1a of the canonical name (same width and hash family as sid_t / ref).  Zero is
    reserved as the invalid id.  Uniqueness is guaranteed by the BUILD, not by the width: the
    harvester proves the complete name set is collision-free and fails the build on any clash,
    so a runtime collision cannot occur.  The width only sets how often that check forces a
    rename -- ~0.01% at 1k names, ~1% at 10k; negligible at this engine's scale.
==============================================================================================*/

typedef u32 rid_t;

#define RID_INVALID           ( ( rid_t )0 )

/*==============================================================================================
    Limits

    Static storage sized at compile time; the registry never allocates.
==============================================================================================*/

#define RES_MAX_ENTRIES       8192                       /* distinct names */
#define RES_NAME_POOL_SIZE    ( 16 * 1024 )              /* bytes of copied name text */
#define RES_NAME_MAX          255                        /* canonical name length, excl. NUL */
#define RES_HASH_SIZE         16384                      /* power of two, >= 2x RES_MAX_ENTRIES */
#define RES_HASH_MASK         ( RES_HASH_SIZE - 1 )

/*==============================================================================================
    Canonical form

    A name is compared and hashed in canonical form: ASCII lowercase with '/' separators.
    Backslashes fold to '/'.  Nothing else is rewritten -- a leading or trailing slash or a
    doubled separator is a different name, and the build-time scanner is where that gets
    flagged, not here.  The fold is a single-char function so the hash below and the pool
    copy in res.c can never disagree.
==============================================================================================*/

static inline char
res_canon_char( char c )
{
    if ( c >= 'A' && c <= 'Z' ) return ( char )( c + 32 );
    if ( c == '\\' )            return '/';
    return c;
}

/*==============================================================================================
    res_hash_name -- rid_t of a name

    Header-only so DLL modules and offline tools (the build-time scanner, the cooker) compute
    the identical id with no link dependency on res.  A zero result is remapped to 1 so that
    RID_INVALID can never be produced by a real name.
==============================================================================================*/

static inline rid_t
res_hash_name( const char* s )
{
    u32 h = 2166136261u;                                 /* FNV-1a 32 offset basis */
    while ( *s )
    {
        h ^= ( u32 )( u8 )res_canon_char( *s++ );
        h *= 16777619u;                                  /* FNV-1a 32 prime */
    }
    return h ? ( rid_t )h : ( rid_t )1;
}

/*==============================================================================================
    RID( "name" ) -- the one door from a literal to an id

    The only sanctioned way source code turns a name into a rid_t.  The "" prefix makes
    anything but a string literal a compile error, which is what lets a build-time scanner
    find every reference by looking for one token.  Never wrap a runtime string in RID().
==============================================================================================*/

#define RID( lit )            res_hash_name( "" lit )

/*==============================================================================================
    Resource tables

    A module's declared name set, emitted by the build from the RID() tokens in its sources
    and pointed at from mod_desc_t.res_table (MOD_RES_TABLE).  The res library registers the
    table when the module's pre_init hook fires.  Entries are plain literals living in the
    module image; the registry copies them.
==============================================================================================*/

typedef struct res_entry_s
{
    const char* name;        // logical name as written at the reference site

} res_entry_t;

typedef struct res_table_s
{
    const res_entry_t* entries;    // count entries, any order, duplicates allowed
    u32                count;

} res_table_t;

/*==============================================================================================
    Callbacks
==============================================================================================*/

/* Visitor for res_each: one call per registered name, in registration order. */
typedef void ( *res_each_fn )( rid_t id, const char* name, void* user );

// clang-format on
/*============================================================================================*/
#endif    // RES_H
