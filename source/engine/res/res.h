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
    name to a cooked file; source code never does.  The convention it resolves by: a name is
    the path of its source file under a content root, minus the extension -- "ui/icon/save"
    is content/ui/icon/save.png, cooked to ui/icon/save.tex.  A project's content/ shadows
    the engine's, name by name.

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

    Storage grows on demand and holds no more than the catalogue needs, so the two INIT sizes
    are only where doubling starts -- a first allocation small enough that a host with a
    handful of names pays almost nothing.  A catalogue costs 16 bytes per entry (a 12-byte
    slot plus the two hash buckets it is budgeted) on top of its name and path text.

    RES_MAX_ENTRIES is a hard ceiling, not a budget, and exceeding it is a clean registration
    failure.  It is the largest power of two whose slot index plus one still fits the u16 hash
    bucket -- the doubling keeps every table size a power of two, which the probe mask needs.
==============================================================================================*/

#define RES_INIT_ENTRIES      64                         // first slot allocation; doubles from here
#define RES_INIT_POOL         1024                       // first name-pool allocation, in bytes
#define RES_MAX_ENTRIES       32768                      // distinct names, hard u16 bucket ceiling
#define RES_NAME_MAX          255                        // canonical name length, excl. NUL

/*==============================================================================================
    Canonical form

    A name is compared and hashed in canonical form: ASCII lowercase with '/' separators.
    Backslashes fold to '/'.  Nothing else is rewritten.  A TRAILING slash means the name is
    a subtree ("ui/icon/": everything beneath it), which is a different resource from the
    leaf "ui/icon" and hashes differently, so the two can never share an id.  A leading
    slash or a doubled separator is malformed, and the build-time scanner is where that gets
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

    FNV-1a is a running state, so the id of a name is also the hash state after its bytes:
    res_hash_child continues from a subtree's id over a leaf and lands on the same id
    res_hash_name gives the joined name, with no string built.  That is what makes a
    RES_TREE() handle usable at runtime: names composed from a listing or an enum index
    resolve from the tree id and the leaf text alone.

    The zero remap is the one seam in that identity.  A subtree whose raw state is 0 hands
    out id 1, and continuing from 1 diverges from continuing from 0, so the scanner refuses
    such a subtree name at build time (a one-in-2^32 rename).  Subtrees composed at runtime
    carry the same odds unchecked.
==============================================================================================*/

#define RES_HASH_BASIS        2166136261u                // FNV-1a 32 offset basis
#define RES_HASH_PRIME        16777619u                  // FNV-1a 32 prime

/* Raw FNV-1a state after folding `s` onto `h`; no zero remap. */
static inline u32
res_hash_step( u32 h, const char* s )
{
    while ( *s )
    {
        h ^= ( u32 )( u8 )res_canon_char( *s++ );
        h *= RES_HASH_PRIME;
    }
    return h;
}

static inline rid_t
res_hash_name( const char* s )
{
    u32 h = res_hash_step( RES_HASH_BASIS, s );
    return h ? ( rid_t )h : ( rid_t )1;
}

/* rid_t of `leaf` beneath the subtree `tree`: res_hash_child( RES_TREE( "ui/icon" ), "save" )
   == RID( "ui/icon/save" ).  `leaf` may itself hold separators, and a trailing slash on it
   yields a nested subtree id.  This is the runtime counterpart of RID(): the scanner cannot
   see the leaf, so the subtree must have been declared through RES_TREE(). */
static inline rid_t
res_hash_child( rid_t tree, const char* leaf )
{
    u32 h = res_hash_step( ( u32 )tree, leaf );
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
    RES_TREE( "prefix" ) -- the escape hatch for names composed at runtime

    Declares that every name beneath "prefix" may be loaded: code that builds names from a
    directory listing or an enum-indexed table names the subtree once here instead of each
    leaf.  The macro appends the subtree's trailing slash itself -- RES_TREE( "ui/icon" )
    is the resource "ui/icon/" -- and the scanner records the same spelling, so the author
    never writes the slash and a stray one on either macro stays a build error.  The build
    expands the subtree against the content root; the leaves never appear in source.

    Evaluates to the subtree's own rid_t, so it can also be held as a group handle.  Same
    rule as RID(): string literal only.
==============================================================================================*/

#define RES_TREE( lit )       res_hash_name( "" lit "/" )

/*==============================================================================================
    Resource tables

    An image's declared name set, emitted by the build from the RID() / RES_TREE() tokens in
    its sources -- for a DLL module its own units, for an executable its units plus every
    statically linked library -- and pointed at from mod_desc_t.res_table (MOD_RES_TABLE).
    The res library registers the table when the module's pre_init hook fires, hashing each
    name then -- an id is a pure function of the name, so the table carries no id.

    What it does carry is the answer the build alone can give: the cooked file each name
    resolves to, relative to the content root and WITH its extension.  The path is canonical
    like the name -- lowercase, '/' separators -- because the cooker writes it that way
    whatever the source file was called; it is the name plus the cooked extension.  A subtree
    entry has an empty path -- it is a directory, not a file -- and every file beneath it in
    the content root is listed as its own entry, so a runtime-composed child of a RES_TREE()
    resolves to a path too.  Entries are plain literals living in the image; the registry
    copies them.
==============================================================================================*/

typedef struct res_entry_s
{
    const char* name;               // canonical logical name; trailing '/' = subtree
    const char* path;               // cooked file relative to the content root; "" = none

} res_entry_t;

typedef struct res_table_s
{
    const res_entry_t* entries;     // count entries, any order, duplicates allowed
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
