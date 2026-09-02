/*==============================================================================================

    engine/res/res.h - Resource names: the RID() marker, canonical form, hashing, path join.

    Header-only.  There is no res library, module, or runtime table: a resource name is a
    plain string, and everything here is a stateless helper over it.

    A NAME is the path of a file under a content root, minus the extension: "ui/icon/save" is
    content/ui/icon/save.png.  Names and content file names are canonical lowercase with '/'
    separators, so a name is opened as-is and nothing is ever translated between the two.  The
    extension belongs to the loader (it asks for the name plus each extension it accepts), and
    which tree answers -- loose content, a cooked mirror, a shipped pack -- belongs to the
    mounts.

    RID( "ui/icon/save" ) is the one door a literal name passes through in source.  It
    evaluates to the literal itself; its job is to be the single token the build-time scanner
    (res_tool) looks for.  The scanner resolves every marked name against the content roots,
    fails the build with file:line on a name with no file, a misspelled file, or a collision,
    and writes the image's name set to obj/<target>/<target>_res_manifest.txt for the
    packager.  A name that never passes through RID() or RES_TREE() is invisible to the
    build, loads fine, and ships only if something else names it.

==============================================================================================*/
#ifndef RES_H
#define RES_H

#include "orb.h"

// clang-format off
/*==============================================================================================
    Limits
==============================================================================================*/

#define RES_NAME_MAX          255                        // name length, excl. NUL

/*==============================================================================================
    RID( "name" ) / RES_TREE( "prefix" ) -- the scannable markers

    Both evaluate to a string literal and accept nothing else: the "" prefix makes a macro or
    variable argument a compile error, which is what lets the scanner find every reference by
    one token.  Never wrap a runtime string in either.

    RES_TREE declares that every name beneath "prefix" may be loaded -- code that composes
    names from a listing or an enum table names the subtree once instead of each leaf.  It
    evaluates to the prefix WITH its trailing slash ("ui/icon/"), ready to have a leaf
    appended; the author never writes the slash, and the scanner records the same spelling.
    The build expands the subtree against the content roots, so the leaves need not appear in
    source.
==============================================================================================*/

#define RID( lit )            ( "" lit )
#define RES_TREE( lit )       ( "" lit "/" )

/*==============================================================================================
    Canonical form

    A byte is canonical when res_canon_char leaves it alone: no ASCII uppercase, no backslash.
    Names are REQUIRED to be canonical -- the scanner fails the build on a RID() literal that
    is not, and on a content file or directory that is not -- so nothing folds at runtime.
    The fold function exists for the scanner, which matches a misspelled file case-
    insensitively so it can report the spelling it found.
==============================================================================================*/

static inline char
res_canon_char( char c )
{
    if ( c >= 'A' && c <= 'Z' ) return ( char )( c + 32 );
    if ( c == '\\' )            return '/';
    return c;
}

/* True when `s` is a usable name: non-empty, at most RES_NAME_MAX bytes, every byte canonical
   and printable ASCII, no leading, trailing or doubled '/'.  A trailing slash is allowed only
   by RES_TREE's spelling and is accepted here (a subtree is a name).  Loaders call this to
   refuse a runtime-composed name before it reaches fs. */
static inline bool
res_name_ok( const char* s )
{
    if ( !s || !*s || *s == '/' )
        return false;
    u32 n = 0;
    for ( const char* p = s; *p; ++p, ++n )
    {
        if ( n >= RES_NAME_MAX )                       return false;
        if ( *p <= ' ' || *p >= 0x7F || *p == '"' )    return false;
        if ( res_canon_char( *p ) != *p )              return false;
        if ( *p == '/' && p[ 1 ] == '/' )              return false;
    }
    return true;
}

/*==============================================================================================
    res_hash_name -- rid_t of a name

    32-bit FNV-1a over the bytes as written (names are canonical, so there is nothing to fold).
    A key for anything that indexes by name -- the asset service dedups on it, a cooked file
    may carry one -- never an identity the runtime needs to invert: whoever holds a rid_t and
    wants the name back holds the name too (or a sid_t).  Zero is remapped to 1 so RID_INVALID
    is never produced by a real name.  The scanner proves the marked set collision-free per
    image and fails the build otherwise, so two marked names never share a key.
==============================================================================================*/

typedef u32 rid_t;

#define RID_INVALID           ( ( rid_t )0 )
#define RES_HASH_BASIS        2166136261u                // FNV-1a 32 offset basis
#define RES_HASH_PRIME        16777619u                  // FNV-1a 32 prime

static inline rid_t
res_hash_name( const char* s )
{
    u32 h = RES_HASH_BASIS;
    while ( *s )
    {
        h ^= ( u32 )( u8 )*s++;
        h *= RES_HASH_PRIME;
    }
    return h ? ( rid_t )h : ( rid_t )1;
}

/*==============================================================================================
    res_path -- name + extension into a caller buffer

    The path a loader asks fs (or fopen, below a content root) for: "ui/icon/save" + ".png".
    `ext` carries its own leading dot; pass "" for none.  Returns false and writes an empty
    string when the result does not fit in `cap` bytes.
==============================================================================================*/

#define RES_EXT_MAX           16                         // extension incl. dot, excl. NUL
#define RES_PATH_MAX          ( RES_NAME_MAX + RES_EXT_MAX + 1 )   // a res_path result incl. NUL

static inline bool
res_path( char* out, u32 cap, const char* name, const char* ext )
{
    if ( cap == 0 )
        return false;
    u32 o = 0;
    for ( const char* p = name; *p; ++p )
    {
        if ( o + 1 >= cap ) { out[ 0 ] = 0; return false; }
        out[ o++ ] = *p;
    }
    for ( const char* p = ext; *p; ++p )
    {
        if ( o + 1 >= cap ) { out[ 0 ] = 0; return false; }
        out[ o++ ] = *p;
    }
    out[ o ] = 0;
    return true;
}

// clang-format on
/*============================================================================================*/
#endif    // RES_H
