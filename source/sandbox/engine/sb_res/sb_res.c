/*==============================================================================================

    sandbox/engine/sb_res/sb_res.c -- Test sandbox for engine/res (header-only) and the build's
    resource manifest.

    Proves the properties the rest of the design leans on:
      - markers:   RID() evaluates to the literal it wraps; RES_TREE() to the prefix with its
                   trailing slash -- a resource name is a plain string
      - canonical: res_name_ok accepts lowercase '/'-separated names and refuses everything
                   the build would refuse (uppercase, backslash, empty segments, ...)
      - hashing:   res_hash_name is deterministic, never RID_INVALID, and the FNV-1a pair the
                   build's collision check exists for really does collide
      - paths:     res_path joins a name and an extension into a caller buffer and refuses to
                   overflow it
      - harvest:   this executable's resource manifest, written by the build from the RID() /
                   RES_TREE() literals in this file and resolved against content/, holds
                   exactly those names plus the files the subtree expanded to -- and nothing a
                   plain string mentions.  The fixtures live in content/sandbox/res/.

    Exit code is the number of failed checks, so it can gate a build step.  Run from the repo
    root (the manifest is read from build/obj/sb_res/).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/res/res.h"

/*==============================================================================================
    Check helper
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
sb_check( bool ok, const char* what )
{
    s_checks++;
    if ( !ok )
    {
        s_fails++;
        printf( "    FAIL: %s\n", what );
    }
}

static bool
str_eq( const char* a, const char* b )
{
    return a && b && strcmp( a, b ) == 0;
}

/*==============================================================================================
    Markers
==============================================================================================*/

static void
test_markers( void )
{
    printf( "  markers\n" );

    /* RID() is the literal: the same text from any site, usable anywhere a string is. */
    const char* a = RID( "sandbox/res/icon/save" );
    sb_check( str_eq( a, "sandbox/res/icon/save" ), "RID evaluates to its literal" );
    sb_check( str_eq( RID( "sandbox/res/icon/load" ), "sandbox/res/icon/load" ), "a second site, a second literal" );

    /* RES_TREE() appends the subtree slash itself, so a leaf can be appended directly. */
    const char* tree = RES_TREE( "sandbox/res/icon" );
    sb_check( str_eq( tree, "sandbox/res/icon/" ), "RES_TREE evaluates to the prefix with its slash" );

    char child[ RES_PATH_MAX ];
    snprintf( child, sizeof( child ), "%s%s", tree, "never" );
    sb_check( str_eq( child, "sandbox/res/icon/never" ), "a runtime-composed child is prefix + leaf" );

    /* Both are compile-time constants: sizeof sees the literal (a build-resolved name, since
       every marker in this file must have a file under content/). */
    sb_check( sizeof( RID( "sandbox/res/icon/save" ) ) == 22 && sizeof( RES_TREE( "sandbox/res/icon" ) ) == 18,
              "markers are string literals" );
}

/*==============================================================================================
    Canonical form
==============================================================================================*/

static void
test_canonical( void )
{
    printf( "  canonical form\n" );

    sb_check( res_name_ok( "sandbox/res/icon/save" ), "a lowercase '/'-separated name is ok" );
    sb_check( res_name_ok( "a" ), "a single segment is ok" );
    sb_check( res_name_ok( "sandbox/res/icon/" ), "a subtree spelling (trailing slash) is ok" );
    sb_check( res_name_ok( "font/cascadia_mono-2/16.sdf" ), "digits, '_', '-', '.' are ok" );

    sb_check( !res_name_ok( "" ), "empty is refused" );
    sb_check( !res_name_ok( NULL ), "NULL is refused" );
    sb_check( !res_name_ok( "Sandbox/res/icon/save" ), "uppercase is refused" );
    sb_check( !res_name_ok( "sandbox\\res\\icon\\save" ), "backslash is refused" );
    sb_check( !res_name_ok( "/sandbox/res" ), "a leading slash is refused" );
    sb_check( !res_name_ok( "sandbox//res" ), "an empty segment is refused" );
    sb_check( !res_name_ok( "sandbox/res icon" ), "whitespace is refused" );
    sb_check( !res_name_ok( "sandbox/\"res\"" ), "a double quote is refused" );

    char too_long[ RES_NAME_MAX + 2 ];
    memset( too_long, 'a', sizeof( too_long ) - 1 );
    too_long[ sizeof( too_long ) - 1 ] = 0;
    sb_check( !res_name_ok( too_long ), "over RES_NAME_MAX is refused" );
    too_long[ RES_NAME_MAX ] = 0;
    sb_check( res_name_ok( too_long ), "exactly RES_NAME_MAX is ok" );

    sb_check( res_canon_char( 'A' ) == 'a' && res_canon_char( '\\' ) == '/' && res_canon_char( 'z' ) == 'z' &&
                  res_canon_char( '/' ) == '/',
              "res_canon_char folds uppercase and backslash only" );
}

/*==============================================================================================
    Hashing
==============================================================================================*/

static void
test_hash( void )
{
    printf( "  hashing\n" );

    rid_t a = res_hash_name( "sandbox/res/icon/save" );
    sb_check( a == res_hash_name( "sandbox/res/icon/save" ), "same name -> same id" );
    sb_check( a != RID_INVALID, "id is never RID_INVALID" );
    sb_check( a != res_hash_name( "sandbox/res/icon/load" ), "different names -> different ids" );
    sb_check( res_hash_name( "sandbox/res/icon/" ) != res_hash_name( "sandbox/res/icon" ),
              "a subtree spelling and its leaf hash differently" );
    sb_check( res_hash_name( "Sandbox/Res/Icon/Save" ) != a, "the hash folds nothing: spelling is identity" );

    /* Two real names on one FNV-1a 32 id, found by brute force.  The build-time scanner
       refuses this pair in source, which is why the pair is only ever hashed here. */
    sb_check( res_hash_name( "collide/2ae/40b" ) == res_hash_name( "collide/346/339" ),
              "the known collision pair still collides (the build's check has a reason to exist)" );
}

/*==============================================================================================
    Paths
==============================================================================================*/

static void
test_path( void )
{
    printf( "  paths\n" );

    char buf[ 32 ];
    sb_check( res_path( buf, sizeof( buf ), "sandbox/res/icon/save", ".png" ) && str_eq( buf, "sandbox/res/icon/save.png" ),
              "name + extension" );
    sb_check( res_path( buf, sizeof( buf ), "sandbox/res/icon/save", "" ) && str_eq( buf, "sandbox/res/icon/save" ),
              "an empty extension leaves the name" );

    sb_check( res_path( buf, 26, "sandbox/res/icon/save", ".png" ), "exactly fits (25 chars + NUL)" );
    sb_check( !res_path( buf, 25, "sandbox/res/icon/save", ".png" ) && buf[ 0 ] == 0,
              "one byte short: refused, buffer emptied" );
    sb_check( !res_path( buf, 0, "a", "" ), "zero capacity: refused" );
}

/*==============================================================================================
    Harvest: the build wrote this executable's manifest from the literals above

    Nothing in this file names "sandbox/res/icon/never" through a marker except test_markers's
    composition -- and that one is composed at runtime, so the literal that puts it in the
    manifest is the RID() below.  A name spelled only as a plain string (the "plain" one
    below) must NOT be in the manifest.  The RES_TREE() arrives as "sandbox/res/icon/", slash
    included, and brings the files beneath that directory that no source line spells.

    Every name was also resolved against content/ (see content/sandbox/res/readme.md): each
    leaf was proven to be exactly one lowercase file.
==============================================================================================*/

#define SB_MANIFEST "build/obj/sb_res/sb_res_res_manifest.txt"

static const char* const k_expected[] = {
    "sandbox/res/font/mono/16",
    "sandbox/res/icon/",
    "sandbox/res/icon/load",
    "sandbox/res/icon/never",
    "sandbox/res/icon/save",
    "sandbox/res/icon/sub/deep",
    "sandbox/res/icon/tree_only",
};

static void
test_harvest( void )
{
    printf( "  harvest\n" );

    /* The literals that put names into the manifest.  Only their presence in source matters. */
    const char* spelled[] = {
        RID( "sandbox/res/icon/save" ),
        RID( "sandbox/res/icon/load" ),
        RID( "sandbox/res/icon/never" ),
        RID( "sandbox/res/font/mono/16" ),
        RES_TREE( "sandbox/res/icon" ),
    };
    const char* plain = "sandbox/res/plain";    /* never marked: must not be harvested */
    sb_check( spelled[ 0 ] && plain, "literals present" );

    FILE* f = fopen( SB_MANIFEST, "rb" );
    sb_check( f != NULL, "the build wrote " SB_MANIFEST " (run from the repo root)" );
    if ( !f )
        return;

    u32  seen[ ARRAY_COUNT( k_expected ) ] = { 0 };
    u32  entries  = 0;
    bool unknown  = false;
    bool has_plain = false;
    char line[ 1024 ];
    while ( fgets( line, sizeof( line ), f ) )
    {
        char* p = line;
        while ( *p == ' ' || *p == '\t' )
            p++;
        if ( *p == '#' || *p == '\n' || *p == '\r' || *p == 0 )
            continue;    /* comment or blank */

        char* name = p;
        while ( *p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' )
            p++;
        *p = 0;
        entries++;

        bool matched = false;
        for ( u32 i = 0; i < ARRAY_COUNT( k_expected ); ++i )
            if ( strcmp( name, k_expected[ i ] ) == 0 )
            {
                seen[ i ]++;
                matched = true;
            }
        if ( !matched )
        {
            unknown = true;
            printf( "    unexpected manifest entry: %s\n", name );
        }
        if ( strcmp( name, plain ) == 0 )
            has_plain = true;
        sb_check( res_name_ok( name ), "every manifest name is canonical" );
    }
    fclose( f );

    bool all_once = true;
    for ( u32 i = 0; i < ARRAY_COUNT( k_expected ); ++i )
        if ( seen[ i ] != 1 )
        {
            all_once = false;
            printf( "    expected once, saw %u times: %s\n", seen[ i ], k_expected[ i ] );
        }

    sb_check( all_once, "every marked name and every expanded file is listed exactly once" );
    sb_check( !unknown && entries == ARRAY_COUNT( k_expected ),
              "the manifest holds exactly the four RID() leaves, the subtree, and its two expanded files" );
    sb_check( !has_plain, "a plain-string name outside the markers is not harvested" );
}

/*==============================================================================================
    Entry
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "sb_res: resource names\n" );

    test_markers();
    test_canonical();
    test_hash();
    test_path();
    test_harvest();

    printf( "sb_res: %d checks, %d failed\n", s_checks, s_fails );
    return s_fails;
}

/*============================================================================================*/
