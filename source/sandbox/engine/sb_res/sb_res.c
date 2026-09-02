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
      - refs:      the reference section of a cooked file (res_ref.h) round-trips through the
                   header-only writer and reader, every malformed shape is refused, and a walk
                   from a parent file reaches the children it names without any source line
                   spelling them -- the edge the packager follows.

    Exit code is the number of failed checks, so it can gate a build step.  Run from the repo
    root (the manifest is read from build/obj/sb_res/).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/res/res.h"
#include "engine/res/res_ref.h"

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
    Reference sections: what a cooked file says it needs, and the packager's walk over it

    Synthetic cooked files built in memory with the layout every format shares (res_ref.h): a
    header opening with the res_ref_head_t fields, the reference section right after it, then
    a payload.  Nothing here knows any real format, which is the point: the packager reads
    the head, finds the section, and walks the names.

    The walk fixture is three files.  The parent names both children; child a also names
    child b; child b names nothing.  No RID() spells the children -- they are plain strings
    here and absent from this executable's manifest (test_harvest proves the manifest holds
    only the marked set) -- yet the walk from the parent reaches them, and reaches b once.
==============================================================================================*/

/* A stand-in format header: the shared head, then one field of its own. */
typedef struct
{
    u32 magic;
    u32 version;
    u32 ref_count;
    u32 ref_size;
    u32 ref_offset;
    u32 payload_size;

} fake_hdr_t;

RES_REF_HEAD_ASSERT( fake_hdr_t );

#define FAKE_MAGIC   0x454B4146u    /* 'FAKE' */
#define FAKE_PAYLOAD 12u

/* Assemble a fake cooked file: a head claiming (`count`, `size`, `offset`), then `sec_len`
   bytes of `sec`, then the payload.  Returns the byte count written. */
static u32
fake_build( u8* out, u32 count, u32 size, u32 offset, const u8* sec, u32 sec_len )
{
    fake_hdr_t h   = { FAKE_MAGIC, 1, count, size, offset, FAKE_PAYLOAD };
    u32        n   = 0;
    memcpy( out + n, &h, sizeof( h ) );   n += ( u32 )sizeof( h );
    memcpy( out + n, sec, sec_len );      n += sec_len;
    memset( out + n, 0xAB, FAKE_PAYLOAD ); n += FAKE_PAYLOAD;
    return n;
}

/* A well-formed fake file naming `refs`: the section res_ref_write produces, at its natural
   offset, with the head sized to match. */
static u32
fake_build_ok( u8* out, const char* const* refs, u32 count )
{
    u8  sec[ 256 ];
    u32 size = 0;
    if ( !res_ref_measure( refs, count, &size ) || !res_ref_write( sec, sizeof( sec ), refs, count ) )
        return 0;
    return fake_build( out, count, size, ( u32 )sizeof( fake_hdr_t ), sec, size );
}

typedef struct
{
    const char* name;
    u8          bytes[ 512 ];
    u32         size;
    u32         reads;    /* how many times the walk opened this file */

} fake_file_t;

static fake_file_t s_files[ 3 ];

static fake_file_t*
fake_open( const char* name )
{
    for ( u32 i = 0; i < ARRAY_COUNT( s_files ); ++i )
        if ( str_eq( s_files[ i ].name, name ) )
            return &s_files[ i ];
    return NULL;
}

/* The packager's walk, in miniature: visit a name once, read its file's reference section,
   recurse into every name it lists.  `visited` collects the closure in visit order. */
static bool
fake_walk( const char* name, const char** visited, u32* visited_count, u32 cap )
{
    for ( u32 i = 0; i < *visited_count; ++i )
        if ( str_eq( visited[ i ], name ) )
            return true;
    if ( *visited_count >= cap )
        return false;
    visited[ ( *visited_count )++ ] = name;

    fake_file_t* f = fake_open( name );
    if ( !f )
        return false;
    f->reads++;

    const u8* sec;
    u32       size, count;
    if ( !res_ref_locate( f->bytes, f->size, &sec, &size, &count ) )
        return false;

    u32 cursor = 0;
    for ( const char* child; ( child = res_ref_next( sec, size, &cursor ) ) != NULL; )
        if ( !fake_walk( child, visited, visited_count, cap ) )
            return false;
    return true;
}

static void
test_refs( void )
{
    printf( "  reference sections\n" );

    static const char* const two[] = { "sandbox/res/walk/child_a", "sandbox/res/walk/child_b" };
    u32                      size  = 0;

    /* Measure and write: "sandbox/res/walk/child_a" is 24 + NUL, twice = 50, padded to 56. */
    sb_check( res_ref_measure( two, 2, &size ) && size == 56, "two names measure to their padded length" );
    sb_check( res_ref_measure( NULL, 0, &size ) && size == 0, "no names measure to 0" );
    {
        static const char* const bad[] = { "Sandbox/res/walk/child_a" };
        sb_check( !res_ref_measure( bad, 1, &size ) && size == 0, "a non-canonical name is refused at write" );
        static const char* const empty[] = { "" };
        sb_check( !res_ref_measure( empty, 1, &size ), "an empty name is refused at write" );
    }

    u8 sec[ 128 ];
    memset( sec, 0xCC, sizeof( sec ) );
    sb_check( res_ref_write( sec, sizeof( sec ), two, 2 ), "the section writes" );
    sb_check( !res_ref_write( sec, 55, two, 2 ), "a buffer one byte short is refused" );
    sb_check( res_ref_write( sec, 56, two, 2 ), "a buffer of exactly the padded length is enough" );
    sb_check( sec[ 50 ] == 0 && sec[ 55 ] == 0, "padding is zero" );
    sb_check( res_ref_section_ok( sec, 56, 2 ), "the written section validates" );

    /* Iterate back. */
    {
        u32         cursor = 0;
        const char* a      = res_ref_next( sec, 56, &cursor );
        const char* b      = res_ref_next( sec, 56, &cursor );
        const char* end    = res_ref_next( sec, 56, &cursor );
        sb_check( str_eq( a, two[ 0 ] ) && str_eq( b, two[ 1 ] ) && end == NULL, "the names read back in order, then NULL" );
    }

    /* Head bounds. */
    {
        res_ref_head_t h = { FAKE_MAGIC, 1, 2, 56, sizeof( fake_hdr_t ) };
        sb_check( res_ref_head_ok( &h ), "a sane head is ok" );
        h.ref_size = 52;   sb_check( !res_ref_head_ok( &h ), "a size not a multiple of RES_REF_ALIGN is refused" );
        h.ref_size = 0;    sb_check( !res_ref_head_ok( &h ), "a count with no bytes is refused" );
        h.ref_count = 0;   sb_check( res_ref_head_ok( &h ), "no names, no bytes is ok" );
        h.ref_size = 8;    sb_check( !res_ref_head_ok( &h ), "bytes with no names are refused" );
        h.ref_count = RES_REF_MAX + 1; h.ref_size = 56;
        sb_check( !res_ref_head_ok( &h ), "a count past RES_REF_MAX is refused" );
        h.ref_count = 2; h.ref_size = RES_REF_SIZE_MAX + 8;
        sb_check( !res_ref_head_ok( &h ), "a size past RES_REF_SIZE_MAX is refused" );
        h.ref_size = 56; h.ref_offset = 4;
        sb_check( !res_ref_head_ok( &h ), "an offset inside the head is refused" );
    }

    /* Whole-file location, well-formed and every malformed shape. */
    {
        u8        file[ 512 ];
        const u8* got;
        u32       got_size, got_count, n;

        n = fake_build_ok( file, two, 2 );
        sb_check( n && res_ref_locate( file, n, &got, &got_size, &got_count ) && got_count == 2 && got_size == 56
                      && got == file + sizeof( fake_hdr_t ),
                  "a well-formed file locates its section" );

        n = fake_build_ok( file, NULL, 0 );
        sb_check( n && res_ref_locate( file, n, &got, &got_size, &got_count ) && got_count == 0 && got_size == 0,
                  "a file naming nothing locates an empty section" );

        sb_check( !res_ref_locate( file, sizeof( res_ref_head_t ) - 1, &got, &got_size, &got_count ),
                  "a file shorter than the head is refused" );

        n = fake_build( file, 2, 56, 400, sec, 56 );
        sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "a section past the end of the file is refused" );

        n = fake_build( file, 1, 56, sizeof( fake_hdr_t ), sec, 56 );
        sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "a count short of the names present is refused" );

        n = fake_build( file, 3, 56, sizeof( fake_hdr_t ), sec, 56 );
        sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "a count past the names present is refused" );

        {
            u8 dirty[ 56 ];
            memcpy( dirty, sec, 56 );
            dirty[ 55 ] = 1;
            n = fake_build( file, 2, 56, sizeof( fake_hdr_t ), dirty, 56 );
            sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "nonzero padding is refused" );

            memcpy( dirty, sec, 56 );
            memset( dirty + 49, 'x', 7 );    /* the second name's NUL and the padding: it never ends */
            n = fake_build( file, 2, 56, sizeof( fake_hdr_t ), dirty, 56 );
            sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "an unterminated last name is refused" );

            memcpy( dirty, sec, 56 );
            dirty[ 0 ] = 'S';
            n = fake_build( file, 2, 56, sizeof( fake_hdr_t ), dirty, 56 );
            sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "a non-canonical name is refused at read" );
        }

        {
            /* "a/b" + NUL = 4 bytes, padded to 8.  A head claiming 16 has a whole RES_REF_ALIGN of
               zero slack after the natural padding, which is not padding but bytes no count
               admits to. */
            static const char* const one[] = { "a/b" };
            u8                       loose[ 16 ] = { 0 };
            res_ref_write( loose, sizeof( loose ), one, 1 );
            n = fake_build( file, 1, 16, sizeof( fake_hdr_t ), loose, 16 );
            sb_check( !res_ref_locate( file, n, &got, &got_size, &got_count ), "a whole alignment unit of slack is refused" );
            n = fake_build( file, 1, 8, sizeof( fake_hdr_t ), loose, 8 );
            sb_check( res_ref_locate( file, n, &got, &got_size, &got_count ) && got_size == 8, "the natural padding is accepted" );
        }
    }

    /* The walk. */
    {
        static const char* const parent_refs[] = { "sandbox/res/walk/child_a", "sandbox/res/walk/child_b" };
        static const char* const a_refs[]      = { "sandbox/res/walk/child_b" };

        memset( s_files, 0, sizeof( s_files ) );
        s_files[ 0 ].name = "sandbox/res/walk/parent";
        s_files[ 0 ].size = fake_build_ok( s_files[ 0 ].bytes, parent_refs, 2 );
        s_files[ 1 ].name = "sandbox/res/walk/child_a";
        s_files[ 1 ].size = fake_build_ok( s_files[ 1 ].bytes, a_refs, 1 );
        s_files[ 2 ].name = "sandbox/res/walk/child_b";
        s_files[ 2 ].size = fake_build_ok( s_files[ 2 ].bytes, NULL, 0 );
        sb_check( s_files[ 0 ].size && s_files[ 1 ].size && s_files[ 2 ].size, "the three fixture files build" );

        const char* visited[ 8 ];
        u32         visited_count = 0;
        bool        ok = fake_walk( "sandbox/res/walk/parent", visited, &visited_count, ARRAY_COUNT( visited ) );
        sb_check( ok, "the walk completes" );
        sb_check( visited_count == 3, "the walk reaches exactly the parent and its two children" );
        sb_check( visited_count == 3 && str_eq( visited[ 0 ], "sandbox/res/walk/parent" )
                      && str_eq( visited[ 1 ], "sandbox/res/walk/child_a" )
                      && str_eq( visited[ 2 ], "sandbox/res/walk/child_b" ),
                  "depth first, in the order the parent named them" );
        sb_check( s_files[ 2 ].reads == 1, "a child named twice (by the parent and by its sibling) is read once" );

        /* Break the parent's section and the walk refuses rather than guesses. */
        s_files[ 0 ].bytes[ sizeof( fake_hdr_t ) ] = 'S';
        visited_count = 0;
        sb_check( !fake_walk( "sandbox/res/walk/parent", visited, &visited_count, ARRAY_COUNT( visited ) ),
                  "a corrupt section stops the walk" );
    }
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
    test_refs();

    printf( "sb_res: %d checks, %d failed\n", s_checks, s_fails );
    return s_fails;
}

/*============================================================================================*/
