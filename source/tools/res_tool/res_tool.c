/*==============================================================================================

    res_tool.c - Resource reference harvester + name resolver (build-time)

    Usage:
        res_tool -list <units.txt> -out <table.c> -name <symbol>
                 [-inc <dir>]... [-root <dir>]... [-deps <file>] [-silent]

    Reads a list of translation units (one path per line; '#' starts a comment), follows
    every #include "..." reachable from them -- unity fragments and headers alike -- and
    collects the names spelled through RID( "..." ) and RES_TREE( "..." ).  Each name is
    then resolved against the content roots to the file it stands for, and the set is
    written as a C table of canonical names and cooked relative paths:

        const res_table_t g_<symbol>_res_table;

    which the executable or DLL compiles in and hands to the resource catalogue through its
    module descriptor (MOD_RES_TABLE).  build_tool invokes this once per image, over the
    image's own units plus those of every statically linked dependency; see
    build_gen_res_table in build_tool_09_exec.c.

    RESOLUTION.  A name is the path of its source file under a content root, minus the
    extension: "ui/icon/save" is <root>/ui/icon/save.<ext> for exactly one <ext>.  Matching
    is case-insensitive segment by segment (the name is canonical lowercase; the source file
    is spelled however the author spelled it).  The recorded path is CANONICAL: the name
    plus the cooked extension, lowercase with '/' separators, because that is the path the
    cooker writes -- source spelling never reaches the cooked tree or the runtime.  The
    cooked extension comes from the source extension through engine/res/res_cook.h -- the
    same table asset_tool writes files with -- and a .recipe file names its cooked kind on a
    "kind <word>" line.  Roots are searched in the order given and the first that holds the
    name wins, so a project's content/ shadows the engine's name by name, whatever the two
    extensions are.

    A RES_TREE prefix is recorded with a trailing slash, exactly as the macro hashes it, so
    "ui/icon/" (the subtree) and "ui/icon" (a leaf) are two entries with two ids.  The
    subtree names a directory: it carries no path itself, and every file beneath it in any
    root becomes an entry of its own, so a child composed at runtime (res_hash_child) finds
    a path in the catalogue.  Two roots holding the same subtree merge, first root winning
    per name.

    Errors, each reported at the RID / RES_TREE site before the tool exits non-zero so one
    run shows the whole list: a name with no file under any root; a name matching two files
    in one directory (save.png beside save.jpg); a leaf name that is only a directory (use
    RES_TREE); a subtree with no directory; a recipe with no usable "kind" line; a malformed
    name (empty segment, leading or trailing separator, whitespace, non-ASCII); a RID()
    whose argument is not a string literal; and two names hashing to one rid_t (the
    catalogue's uniqueness guarantee is proven here, with the same res_hash_name the runtime
    uses).

    -deps writes every directory listed and every recipe read, one path per line, so
    build_tool can tell when a content change (a file added, renamed, or removed; a recipe
    edited) has made the image's table stale even though no source changed.  A root that
    did not exist is written with a leading '!': the table is stale the moment it appears.

    Include resolution tries the including file's directory first and then each -inc root,
    in order -- the same search the compiler performs for a quoted include.  Angle-bracket
    includes and unresolved paths are skipped: those are system and SDK headers, which
    cannot hold engine resource names.  Conditional compilation is not evaluated, so both
    arms of a platform #ifdef are scanned; an over-inclusion there is harmless.

    Standalone C11: reads files with stdio and lists directories with the OS API, links
    nothing.  It includes engine/res/res.h for the hash and the canonical fold only, so the
    ids it computes are the runtime's.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/res/res.h"
#include "engine/res/res_cook.h"

#if defined( _WIN32 )
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
#endif

// clang-format off
/*==============================================================================================
    Limits
==============================================================================================*/

#define RT_MAX_PATH         1024
#define RT_MAX_INC_DIRS     16
#define RT_MAX_ROOTS        8
#define RT_TOOL             "res_tool"

/*==============================================================================================
    Scanned file
==============================================================================================*/

typedef struct rt_file_s
{
    char*    path;      // normalized path, as opened
    char*    key;       // dedupe key: normalized, case-folded on Windows

} rt_file_t;

/*==============================================================================================
    Harvested entry

    One per canonical name.  An explicit entry comes from a RID / RES_TREE site; an expanded
    entry is a file found beneath a RES_TREE directory and points back at that site through
    `via`.  `path` is filled by resolution and stays "" for a subtree.
==============================================================================================*/

typedef struct rt_entry_s
{
    char*    name;      // canonical name (res_canon_char applied); trailing '/' = subtree
    char*    spelled;   // name as written at the first site that mentioned it
    char*    path;      // cooked file relative to its root, canonical lowercase; "" = none
    rid_t    id;        // res_hash_name( name )
    int      file;      // index into g_files of the site
    int      line;      // line of the site
    int      via;       // index of the subtree entry this was expanded from, or -1
    int      root;      // index into g_roots of the root that resolved it, or -1
    bool     tree;      // a subtree (name ends in '/')

} rt_entry_t;

/*==============================================================================================
    Listed directory
==============================================================================================*/

typedef struct rt_dirent_s
{
    char*    name;      // entry name as spelled on disk
    bool     is_dir;

} rt_dirent_t;

typedef struct rt_dir_s
{
    char*         path;     // normalized path, as opened
    char*         key;      // dedupe key (see path_key)
    rt_dirent_t*  ents;
    int           count;
    bool          exists;

} rt_dir_t;

static rt_file_t*  g_files;             // files to scan, in discovery order; also the work queue
static int         g_file_count;        // index of the next file to scan; g_file_count == g_file_cap means done
static int         g_file_cap;          // capacity of g_files

static rt_entry_t* g_entries;
static int         g_entry_count;
static int         g_entry_cap;
static int*        g_entry_hash;        // open addressing over entry ids -> entry index + 1
static int         g_entry_hash_size;   // power of two

static rt_dir_t*   g_dirs;
static int         g_dir_count;
static int         g_dir_cap;

static const char* g_inc_dirs[ RT_MAX_INC_DIRS ];
static int         g_inc_dir_count;

static const char* g_roots[ RT_MAX_ROOTS ];
static int         g_root_count;

static char**      g_deps;              // paths the table depends on, for -deps
static int         g_dep_count;
static int         g_dep_cap;

static int         g_error_count;

/*==============================================================================================
    Helpers
==============================================================================================*/

static void*
rt_xrealloc( void* p, size_t n )
{
    void* q = realloc( p, n );
    if ( !q )
    {
        fprintf( stderr, "[" RT_TOOL "] out of memory\n" );
        exit( 2 );
    }
    return q;
}

static char*
rt_strdup( const char* s, size_t n )
{
    char* d = ( char* )rt_xrealloc( NULL, n + 1 );
    memcpy( d, s, n );
    d[ n ] = 0;
    return d;
}

static void
rt_error( const char* path, int line, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    if ( path )
        fprintf( stderr, "%s(%d): error: ", path, line );
    else
        fprintf( stderr, "[" RT_TOOL "] error: " );
    vfprintf( stderr, fmt, args );
    fputc( '\n', stderr );
    va_end( args );
    g_error_count++;
}

/* True when argv[i] has a following value that is not itself a flag. */
static bool
arg_has_value( int argc, char** argv, int i )
{
    return i + 1 < argc && argv[ i + 1 ][ 0 ] != '-';
}

static const char*
path_basename( const char* s )
{
    const char* p = s;
    for ( const char* c = s; *c; ++c )
        if ( *c == '/' || *c == '\\' )
            p = c + 1;
    return p;
}

/* Extension of a file name (past the last '.'), or "" when it has none.  A leading dot is
   not an extension: ".gitkeep" has stem ".gitkeep". */
static const char*
name_ext( const char* name )
{
    const char* dot = NULL;
    for ( const char* p = name + ( name[ 0 ] == '.' ? 1 : 0 ); *p; ++p )
        if ( *p == '.' )
            dot = p;
    return dot ? dot + 1 : name + strlen( name );
}

/* True when the on-disk spelling `disk` folds to the canonical `canon` over n bytes and
   ends there. */
static bool
name_folds_to( const char* disk, const char* canon, size_t n )
{
    for ( size_t i = 0; i < n; ++i )
        if ( disk[ i ] == 0 || res_canon_char( disk[ i ] ) != canon[ i ] )
            return false;
    return disk[ n ] == 0;
}

/*==============================================================================================

    ...

==============================================================================================*/

static void
dep_add( const char* path, bool missing )
{
    if ( g_dep_count == g_dep_cap )
    {
        g_dep_cap = g_dep_cap ? g_dep_cap * 2 : 64;
        g_deps    = ( char** )rt_xrealloc( g_deps, ( size_t )g_dep_cap * sizeof( char* ) );
    }
    size_t n = strlen( path );
    char*  d = ( char* )rt_xrealloc( NULL, n + 2 );
    d[ 0 ]   = missing ? '!' : 0;
    memcpy( d + ( missing ? 1 : 0 ), path, n + 1 );
    g_deps[ g_dep_count++ ] = d;
}

/*==============================================================================================
    Paths

    Normalized form: forward slashes, no "." segments, ".." folded into its parent where one
    exists.  Two spellings of one file must land on the same key or the file is scanned
    twice; on Windows the key is additionally case-folded.
==============================================================================================*/

static void
path_normalize( const char* in, char* out, size_t cap )
{
    char   tmp[ RT_MAX_PATH ];
    size_t n = strlen( in );
    if ( n >= sizeof( tmp ) )
        n = sizeof( tmp ) - 1;
    memcpy( tmp, in, n );
    tmp[ n ] = 0;
    for ( char* p = tmp; *p; ++p )
        if ( *p == '\\' )
            *p = '/';

    /* Segment walk, writing into out. A drive prefix or leading slash is kept verbatim. */
    size_t o = 0;
    const char* p = tmp;
    if ( n >= 2 && tmp[ 1 ] == ':' )
    {
        out[ o++ ] = tmp[ 0 ];
        out[ o++ ] = ':';
        p += 2;
    }
    if ( *p == '/' )
    {
        out[ o++ ] = '/';
        p++;
    }
    size_t root = o;

    while ( *p )
    {
        const char* seg = p;
        while ( *p && *p != '/' )
            p++;
        size_t len = ( size_t )( p - seg );
        if ( *p == '/' )
            p++;

        if ( len == 0 || ( len == 1 && seg[ 0 ] == '.' ) )
            continue;

        if ( len == 2 && seg[ 0 ] == '.' && seg[ 1 ] == '.' )
        {
            /* Fold into the previous segment unless there is none, or it is itself "..". */
            if ( o > root )
            {
                size_t k = o;
                if ( k > root && out[ k - 1 ] == '/' )
                    k--;
                size_t start = k;
                while ( start > root && out[ start - 1 ] != '/' )
                    start--;
                bool prev_is_dots = ( k - start == 2 && out[ start ] == '.' && out[ start + 1 ] == '.' );
                if ( !prev_is_dots )
                {
                    o = start;
                    continue;
                }
            }
        }

        if ( o + len + 2 >= cap )
            break;

        memcpy( out + o, seg, len );

        o += len;
        out[ o++ ] = '/';
    }

    if ( o > root && out[ o - 1 ] == '/' )
        o--;
    out[ o ] = 0;
}

static void
path_dirname( const char* path, char* out, size_t cap )
{
    size_t n = strlen( path );
    while ( n > 0 && path[ n - 1 ] != '/' )
         n--;

    if ( n > 1 )
         n--;       /* drop the separator, keep a bare "/" root */

    if ( n >= cap )
         n = cap - 1;

    memcpy( out, path, n );
    out[ n ] = 0;
    if ( n == 0 )
    {
        out[ 0 ] = '.';
        out[ 1 ] = 0;
    }
}

static void
path_key( const char* norm, char* out, size_t cap )
{
    size_t n = strlen( norm );
    if ( n >= cap )
         n = cap - 1;

    for ( size_t i = 0; i < n; ++i )
    {
        char c = norm[ i ];
    #if defined( _WIN32 )
        if ( c >= 'A' && c <= 'Z' )
             c = ( char )( c + 32 );
    #endif
        out[ i ] = c;
    }
    out[ n ] = 0;
}

static bool
file_exists( const char* path )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;

    fclose( f );
    return true;
}

/* Whole file into a NUL-terminated heap buffer. NULL when it cannot be read. */
static char*
file_read( const char* path, size_t* out_size )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return NULL;
    fseek( f, 0, SEEK_END );
    long len = ftell( f );
    fseek( f, 0, SEEK_SET );
    if ( len < 0 )
    {
        fclose( f );
        return NULL;
    }
    char* buf = ( char* )rt_xrealloc( NULL, ( size_t )len + 1 );
    size_t got = fread( buf, 1, ( size_t )len, f );
    fclose( f );
    buf[ got ] = 0;
    *out_size = got;
    return buf;
}

/*==============================================================================================
    Directory listing

    Each directory is read from the OS once and cached, keyed like a scanned file.  Entries
    whose name starts with '.' are not content ("." and ".." included, along with .gitkeep
    and tool caches).  Every directory listed -- present or not -- is a dependency of the
    table, because a change in what it holds can change the table.
==============================================================================================*/

static void
dir_push_ent( rt_dir_t* d, const char* name, bool is_dir )
{
    if ( name[ 0 ] == '.' )
        return;
    d->ents = ( rt_dirent_t* )rt_xrealloc( d->ents, ( size_t )( d->count + 1 ) * sizeof( rt_dirent_t ) );
    d->ents[ d->count ].name   = rt_strdup( name, strlen( name ) );
    d->ents[ d->count ].is_dir = is_dir;
    d->count++;
}

static void
dir_read( rt_dir_t* d )
{
#if defined( _WIN32 )
    char pattern[ RT_MAX_PATH + 4 ];
    snprintf( pattern, sizeof( pattern ), "%s/*", d->path );
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA( pattern, &fd );
    if ( h == INVALID_HANDLE_VALUE )
        return;
    d->exists = true;
    do {
        dir_push_ent( d, fd.cFileName, ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 );
    } while ( FindNextFileA( h, &fd ) );
    FindClose( h );
#else
    DIR* dir = opendir( d->path );
    if ( !dir )
        return;
    d->exists = true;
    struct dirent* e;
    while ( ( e = readdir( dir ) ) != NULL )
    {
        char        full[ RT_MAX_PATH ];
        struct stat st;
        snprintf( full, sizeof( full ), "%s/%s", d->path, e->d_name );
        bool is_dir = ( stat( full, &st ) == 0 ) && S_ISDIR( st.st_mode );
        dir_push_ent( d, e->d_name, is_dir );
    }
    closedir( dir );
#endif
}

static const rt_dir_t*
dir_list( const char* raw_path )
{
    char norm[ RT_MAX_PATH ];
    char key[ RT_MAX_PATH ];
    path_normalize( raw_path, norm, sizeof( norm ) );
    path_key( norm, key, sizeof( key ) );

    for ( int i = 0; i < g_dir_count; ++i )
        if ( strcmp( g_dirs[ i ].key, key ) == 0 )
            return &g_dirs[ i ];

    if ( g_dir_count == g_dir_cap )
    {
        g_dir_cap = g_dir_cap ? g_dir_cap * 2 : 64;
        g_dirs    = ( rt_dir_t* )rt_xrealloc( g_dirs, ( size_t )g_dir_cap * sizeof( rt_dir_t ) );
    }
    rt_dir_t* d = &g_dirs[ g_dir_count++ ];
    memset( d, 0, sizeof( *d ) );
    d->path = rt_strdup( norm, strlen( norm ) );
    d->key  = rt_strdup( key, strlen( key ) );
    dir_read( d );
    dep_add( d->path, !d->exists );
    return d;
}

/* The entry of `d` whose on-disk name folds to canonical `seg` (n bytes) and is/is not a
   directory as asked; NULL when none.  `second` receives a further match, which is an
   ambiguity on a case-sensitive filesystem (Save.png beside save.png). */
static const rt_dirent_t*
dir_find( const rt_dir_t* d, const char* seg, size_t n, bool want_dir, const rt_dirent_t** second )
{
    const rt_dirent_t* found = NULL;
    if ( second )
        *second = NULL;
    for ( int i = 0; i < d->count; ++i )
    {
        const rt_dirent_t* e = &d->ents[ i ];
        if ( e->is_dir != want_dir || !name_folds_to( e->name, seg, n ) )
             continue;

        if ( found == NULL ) 
             found = e;

        else if ( second && !*second )
            *second = e;
    }
    return found;
}

/*==============================================================================================
    File queue

    Files are appended as they are discovered and scanned in order, so the array doubles
    as the work queue: the scan index chases g_file_count until they meet.
==============================================================================================*/

static int
file_add( const char* raw_path )
{
    char norm[ RT_MAX_PATH ];
    char key[ RT_MAX_PATH ];
    path_normalize( raw_path, norm, sizeof( norm ) );
    path_key( norm, key, sizeof( key ) );

    for ( int i = 0; i < g_file_count; ++i )
        if ( strcmp( g_files[ i ].key, key ) == 0 )
            return i;

    if ( g_file_count == g_file_cap )
    {
        g_file_cap = g_file_cap ? g_file_cap * 2 : 256;
        g_files    = ( rt_file_t* )rt_xrealloc( g_files, ( size_t )g_file_cap * sizeof( rt_file_t ) );
    }
    rt_file_t* f = &g_files[ g_file_count ];
    memset( f, 0, sizeof( *f ) );
    f->path = rt_strdup( norm, strlen( norm ) );
    f->key  = rt_strdup( key, strlen( key ) );
    return g_file_count++;
}

/* Resolve a quoted include against the including file's directory, then the -inc roots. */
static void
include_add( const char* from_path, const char* inc )
{
    char dir[ RT_MAX_PATH ];
    char cand[ RT_MAX_PATH ];

    path_dirname( from_path, dir, sizeof( dir ) );
    snprintf( cand, sizeof( cand ), "%s/%s", dir, inc );
    if ( file_exists( cand ) )
    {
        file_add( cand );
        return;
    }
    for ( int i = 0; i < g_inc_dir_count; ++i )
    {
        snprintf( cand, sizeof( cand ), "%s/%s", g_inc_dirs[ i ], inc );
        if ( file_exists( cand ) )
        {
            file_add( cand );
            return;
        }
    }
    /* Not ours (system, SDK, or vendored elsewhere): nothing to scan. */
}

/*==============================================================================================
    Names
==============================================================================================*/

/* Why a name is malformed, or NULL when it is acceptable. max is the longest canonical form
   allowed, one less for a subtree since the slash the macro appends counts too. */

static const char*
name_check( const char* s, size_t max )
{
    size_t n = strlen( s );
    if ( n == 0 )
        return "name is empty";
    if ( n > max )
        return "name is longer than RES_NAME_MAX";
    for ( size_t i = 0; i < n; ++i )
    {
        unsigned char c = ( unsigned char )s[ i ];
        if ( c <= ' ' || c >= 0x7F )
            return "name contains whitespace, a control character, or a non-ASCII byte";
        if ( c == '"' )
            return "name contains a double quote";
        bool sep = ( c == '/' || c == '\\' );
        if ( sep && ( i == 0 || i + 1 == n ) )
            return "name has a leading or trailing separator";
        if ( sep && ( s[ i + 1 ] == '/' || s[ i + 1 ] == '\\' ) )
            return "name has an empty segment (doubled separator)";
    }
    return NULL;
}

/* Index of the entry named `canon`, or -1.  Keyed by id; a hash collision between two
   different names keeps both, side by side, for check_collisions to report. */

static int
entry_find( const char* canon, rid_t id )
{
    if ( g_entry_hash_size == 0 )
        return -1;
    u32 mask = ( u32 )g_entry_hash_size - 1;
    for ( u32 b = ( u32 )id & mask;; b = ( b + 1 ) & mask )
    {
        int slot1 = g_entry_hash[ b ];
        if ( slot1 == 0 )
            return -1;
        if ( strcmp( g_entries[ slot1 - 1 ].name, canon ) == 0 )
            return slot1 - 1;
    }
}

static void
entry_hash_insert( int index )
{
    u32 mask = ( u32 )g_entry_hash_size - 1;
    u32 b    = ( u32 )g_entries[ index ].id & mask;
    while ( g_entry_hash[ b ] )
        b = ( b + 1 ) & mask;
    g_entry_hash[ b ] = index + 1;
}

/* Append an entry (the caller has checked it is new).  Returns its index. */

static int
entry_push( const char* canon, size_t n, const char* spelled, bool tree, int file, int line, int via )
{
    if ( g_entry_count == g_entry_cap )
    {
        g_entry_cap = g_entry_cap ? g_entry_cap * 2 : 64;
        g_entries   = ( rt_entry_t* )rt_xrealloc( g_entries, ( size_t )g_entry_cap * sizeof( rt_entry_t ) );
    }
    if ( ( g_entry_count + 1 ) * 2 > g_entry_hash_size )
    {
        g_entry_hash_size = g_entry_hash_size ? g_entry_hash_size * 2 : 256;
        g_entry_hash      = ( int* )rt_xrealloc( g_entry_hash, ( size_t )g_entry_hash_size * sizeof( int ) );
        memset( g_entry_hash, 0, ( size_t )g_entry_hash_size * sizeof( int ) );
        for ( int i = 0; i < g_entry_count; ++i )
            entry_hash_insert( i );
    }

    rt_entry_t* e = &g_entries[ g_entry_count ];
    e->name    = rt_strdup( canon, n );
    e->spelled = rt_strdup( spelled, strlen( spelled ) );
    e->path    = rt_strdup( "", 0 );
    e->id      = res_hash_name( canon );
    e->file    = file;
    e->line    = line;
    e->via     = via;
    e->root    = -1;
    e->tree    = tree;
    entry_hash_insert( g_entry_count );
    return g_entry_count++;
}

/* A RID / RES_TREE site. */

static void
entry_add( const char* spelled, bool tree, int file, int line )
{
    const char* why = name_check( spelled, tree ? RES_NAME_MAX - 1 : RES_NAME_MAX );
    if ( why )
    {
        rt_error( g_files[ file ].path, line, "%s( \"%s\" ): %s", tree ? "RES_TREE" : "RID", spelled, why );
        return;
    }

    /* Canonical form, with the subtree slash RES_TREE() appends in the macro. */
    char canon[ RES_NAME_MAX + 1 ];
    size_t n = strlen( spelled );
    for ( size_t i = 0; i < n; ++i )
        canon[ i ] = res_canon_char( spelled[ i ] );
    if ( tree )
        canon[ n++ ] = '/';
    canon[ n ] = 0;

    /* A subtree id doubles as the hash state res_hash_child continues from, and the zero
       remap in res_hash_name breaks that for exactly one state.  Refuse it here so every
       declared subtree composes correctly at runtime. */
    if ( tree && res_hash_step( RES_HASH_BASIS, canon ) == 0 )
    {
        rt_error( g_files[ file ].path, line,
                  "RES_TREE( \"%s\" ): subtree hashes to the reserved state 0; rename it", spelled );
        return;
    }

    if ( entry_find( canon, res_hash_name( canon ) ) >= 0 )
        return;
    entry_push( canon, n, spelled, tree, file, line, -1 );
}

/*==============================================================================================
    Lexer

    A single pass over the file that knows just enough C to be trusted: it steps over
    comments, string and character literals (so a RID( mentioned in prose or inside a
    string is not a reference), tracks line numbers, and notices two things --

      #include "..."       at the start of a line, queued for scanning
      RID( "..." )         a reference; adjacent literals concatenate as in C
      RES_TREE( "..." )

    Preprocessor lines (a '#' first on the line, continued by trailing backslashes) are
    where the macros themselves are defined, so a RID( there whose argument is not a
    literal is skipped silently.  Anywhere else it is an error: a name reaching RID()
    through a macro or variable is invisible to this scan and would not resolve at runtime.
==============================================================================================*/

typedef struct lex_s
{
    const char* s;          // text
    size_t      n;          // length
    size_t      i;          // cursor
    int         line;       // 1-based line of s[i]
    int         file;       // index into g_files
    bool        bol;        // only whitespace seen since the last newline
    bool        directive;  // inside a preprocessor line

} lex_t;

static bool
lex_is_ident( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

/* Skip spaces, tabs, newlines, backslash continuations and comments. Newlines end a
   directive unless escaped. */
static void
lex_skip_space( lex_t* lx )
{
    for ( ;; )
    {
        if ( lx->i >= lx->n )
            return;
        char c = lx->s[ lx->i ];
        if ( c == '\n' )
        {
            lx->line++;
            lx->i++;
            lx->bol       = true;
            lx->directive = false;
        }
        else if ( c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' )
        {
            lx->i++;
        }
        else if ( c == '\\' && lx->i + 1 < lx->n &&
                  ( lx->s[ lx->i + 1 ] == '\n' ||
                    ( lx->s[ lx->i + 1 ] == '\r' && lx->i + 2 < lx->n && lx->s[ lx->i + 2 ] == '\n' ) ) )
        {
            /* Line continuation: the directive (if any) carries on. */
            lx->i += ( lx->s[ lx->i + 1 ] == '\r' ) ? 3 : 2;
            lx->line++;
        }
        else if ( c == '/' && lx->i + 1 < lx->n && lx->s[ lx->i + 1 ] == '/' )
        {
            while ( lx->i < lx->n && lx->s[ lx->i ] != '\n' )
                lx->i++;
        }
        else if ( c == '/' && lx->i + 1 < lx->n && lx->s[ lx->i + 1 ] == '*' )
        {
            lx->i += 2;
            while ( lx->i < lx->n && !( lx->s[ lx->i ] == '*' && lx->i + 1 < lx->n && lx->s[ lx->i + 1 ] == '/' ) )
            {
                if ( lx->s[ lx->i ] == '\n' )
                    lx->line++;
                lx->i++;
            }
            if ( lx->i < lx->n )
                lx->i += 2;
        }
        else
            return;
    }
}

/* One string literal at the cursor (which must sit on the opening quote) into out, with
   C escapes decoded.  Returns false when the literal is unterminated or too long. */
static bool
lex_string( lex_t* lx, char* out, size_t cap, size_t* io_len )
{
    size_t o = *io_len;
    lx->i++;    /* opening quote */
    while ( lx->i < lx->n )
    {
        char c = lx->s[ lx->i++ ];
        if ( c == '"' )
        {
            out[ o ]  = 0;
            *io_len   = o;
            return true;
        }
        if ( c == '\n' )
            return false;
        if ( c == '\\' && lx->i < lx->n )
        {
            char e = lx->s[ lx->i++ ];
            switch ( e )
            {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case 'a':  c = '\a'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'v':  c = '\v'; break;
                case '0':  c = 0;    break;
                case 'x':
                {
                    int v = 0;
                    while ( lx->i < lx->n )
                    {
                        char h = lx->s[ lx->i ];
                        int  d = ( h >= '0' && h <= '9' ) ? h - '0'
                               : ( h >= 'a' && h <= 'f' ) ? h - 'a' + 10
                               : ( h >= 'A' && h <= 'F' ) ? h - 'A' + 10 : -1;
                        if ( d < 0 )
                            break;
                        v = v * 16 + d;
                        lx->i++;
                    }
                    c = ( char )v;
                    break;
                }
                default:   c = e;    break;    /* \\ \" \' \? and anything unknown */
            }
        }
        if ( o + 1 >= cap )
            return false;
        out[ o++ ] = c;
    }
    return false;
}

/* Called with the cursor just past a RID / RES_TREE identifier that began on `line`.
   Consumes "( lit lit... )" and records the entry; on a shape mismatch leaves the cursor
   where it is so the remaining tokens lex normally. */
static void
lex_reference( lex_t* lx, bool tree, int line )
{
    const char* what = tree ? "RES_TREE" : "RID";
    size_t      save = lx->i;
    int         save_line = lx->line;

    lex_skip_space( lx );
    if ( lx->i >= lx->n || lx->s[ lx->i ] != '(' )
    {
        lx->i    = save;
        lx->line = save_line;
        return;    /* a bare mention of the identifier: a function-pointer name, an #ifdef */
    }
    lx->i++;
    lex_skip_space( lx );

    char   name[ RES_NAME_MAX + 2 ];
    size_t len   = 0;
    int    parts = 0;
    while ( lx->i < lx->n && lx->s[ lx->i ] == '"' )
    {
        if ( !lex_string( lx, name, sizeof( name ), &len ) )
        {
            rt_error( g_files[ lx->file ].path, line, "%s( ... ): string literal is unterminated or too long", what );
            return;
        }
        parts++;
        lex_skip_space( lx );
    }

    if ( parts == 0 || lx->i >= lx->n || lx->s[ lx->i ] != ')' )
    {
        if ( !lx->directive )
            rt_error( g_files[ lx->file ].path, line,
                      "%s( ... ): argument must be a string literal -- a name reaching %s through a"
                      " macro or variable cannot be harvested by the build", what, what );
        return;
    }
    lx->i++;
    entry_add( name, tree, lx->file, line );
}

static void
lex_include( lex_t* lx )
{
    /* Cursor is just past "#include". */
    lex_skip_space( lx );
    if ( lx->i >= lx->n || lx->s[ lx->i ] != '"' )
        return;    /* <system> or a macro operand */

    char   inc[ RT_MAX_PATH ];
    size_t len = 0;
    if ( lex_string( lx, inc, sizeof( inc ), &len ) && len > 0 )
        include_add( g_files[ lx->file ].path, inc );
}

/* Scan one queued file.  g_files is addressed by index throughout, never through a cached
   pointer: an #include found here appends to the queue, and that growth may move the array. */
static void
lex_file( int file )
{
    size_t size = 0;
    char*  text = file_read( g_files[ file ].path, &size );
    if ( !text )
    {
        rt_error( NULL, 0, "cannot read '%s'", g_files[ file ].path );
        return;
    }

    lex_t lx = { .s = text, .n = size, .i = 0, .line = 1, .file = file, .bol = true, .directive = false };

    for ( ;; )
    {
        lex_skip_space( &lx );
        if ( lx.i >= lx.n )
            break;

        char c = lx.s[ lx.i ];

        if ( c == '#' && lx.bol )
        {
            lx.directive = true;
            lx.bol       = false;
            lx.i++;
            lex_skip_space( &lx );
            if ( lx.i + 7 <= lx.n && strncmp( lx.s + lx.i, "include", 7 ) == 0 && !lex_is_ident( lx.s[ lx.i + 7 ] ) )
            {
                lx.i += 7;
                lex_include( &lx );
            }
            continue;
        }

        lx.bol = false;

        if ( c == '"' )
        {
            /* An ordinary string: step over it, escapes included, so its contents are inert. */
            lx.i++;
            while ( lx.i < lx.n && lx.s[ lx.i ] != '"' && lx.s[ lx.i ] != '\n' )
                lx.i += ( lx.s[ lx.i ] == '\\' && lx.i + 1 < lx.n ) ? 2 : 1;
            if ( lx.i < lx.n && lx.s[ lx.i ] == '"' )
                lx.i++;
            continue;
        }

        if ( c == '\'' )
        {
            lx.i++;
            while ( lx.i < lx.n && lx.s[ lx.i ] != '\'' && lx.s[ lx.i ] != '\n' )
                lx.i += ( lx.s[ lx.i ] == '\\' && lx.i + 1 < lx.n ) ? 2 : 1;
            if ( lx.i < lx.n && lx.s[ lx.i ] == '\'' )
                lx.i++;
            continue;
        }

        if ( lex_is_ident( c ) )
        {
            size_t start = lx.i;
            while ( lx.i < lx.n && lex_is_ident( lx.s[ lx.i ] ) )
                lx.i++;
            size_t len = lx.i - start;
            if ( len == 3 && strncmp( lx.s + start, "RID", 3 ) == 0 )
                lex_reference( &lx, false, lx.line );
            else if ( len == 8 && strncmp( lx.s + start, "RES_TREE", 8 ) == 0 )
                lex_reference( &lx, true, lx.line );
            continue;
        }

        lx.i++;
    }

    free( text );
}

/*==============================================================================================
    Resolution

    Walks a name's directory segments through a root one listing at a time, matching each
    segment case-insensitively against what the directory actually holds.  The on-disk
    spelling is used only to open files here; the recorded cooked path is canonical.
==============================================================================================*/

/* Site an entry is reported at: its own for an explicit one, the RES_TREE's for an expanded one. */
static const rt_entry_t*
entry_site( const rt_entry_t* e )
{
    return e->via >= 0 ? &g_entries[ e->via ] : e;
}

static void
resolve_error( const rt_entry_t* e, const char* fmt, ... )
{
    const rt_entry_t* site = entry_site( e );
    char msg[ 2048 ];
    va_list args;
    va_start( args, fmt );
    vsnprintf( msg, sizeof( msg ), fmt, args );
    va_end( args );
    rt_error( g_files[ site->file ].path, site->line, "%s( \"%s\" ): %s",
              site->tree ? "RES_TREE" : "RID", site->spelled, msg );
}

/* The directory `canon_dir` ("" for the root itself, no trailing slash) beneath `root`.
   On success `abs` is the path to open and `rel` its spelling relative to the root ("" or
   "A/B", no trailing slash).  Returns false when any segment is missing. */
static bool
dir_resolve( const char* root, const char* canon_dir, char* abs, size_t abs_cap, char* rel, size_t rel_cap )
{
    snprintf( abs, abs_cap, "%s", root );
    rel[ 0 ] = 0;

    const char* p = canon_dir;
    while ( *p )
    {
        const char* seg = p;
        while ( *p && *p != '/' )
            p++;
        size_t n = ( size_t )( p - seg );
        if ( *p == '/' )
            p++;

        const rt_dir_t* d = dir_list( abs );
        if ( !d->exists )
            return false;
        const rt_dirent_t* e = dir_find( d, seg, n, true, NULL );
        if ( !e )
            return false;

        size_t al = strlen( abs ), rl = strlen( rel );
        snprintf( abs + al, abs_cap - al, "/%s", e->name );
        snprintf( rel + rl, rel_cap - rl, "%s%s", rl ? "/" : "", e->name );
    }
    return dir_list( abs )->exists;
}

/* The cooked kind a recipe file declares on its "kind <word>" line, or RES_KIND_COPY with
   `why` set when the file is unreadable or names no cookable kind. */
static res_kind_t
recipe_kind( const char* abs_path, const char** why )
{
    size_t n    = 0;
    char*  text = file_read( abs_path, &n );
    dep_add( abs_path, false );
    if ( !text )
    {
        *why = "recipe cannot be read";
        return RES_KIND_COPY;
    }

    res_kind_t kind = RES_KIND_COPY;
    bool       seen = false;
    for ( char* line = text; *line; )
    {
        char* end = line;
        while ( *end && *end != '\n' )
            end++;
        char* next = *end ? end + 1 : end;
        *end = 0;
        while ( *line == ' ' || *line == '\t' )
            line++;
        if ( *line && *line != '#' && strncmp( line, "kind", 4 ) == 0 && ( line[ 4 ] == ' ' || line[ 4 ] == '\t' ) )
        {
            char* word = line + 4;
            while ( *word == ' ' || *word == '\t' )
                word++;
            char* we = word;
            while ( *we && *we != ' ' && *we != '\t' && *we != '\r' )
                we++;
            *we  = 0;
            kind = res_kind_from_name( word );
            seen = true;
            break;
        }
        line = next;
    }
    free( text );

    if ( !seen )
        *why = "recipe has no \"kind <word>\" line";
    else if ( kind == RES_KIND_COPY )
        *why = "recipe names an unknown kind (font, image, shader)";
    return kind;
}

/* Cooked relative path for source file `disk_name` in relative directory `rel_dir`: the
   directory and stem folded to canonical form (lowercase, '/'), plus the cooked extension --
   the path asset_tool's job_dst_rel writes.  False with `why` set when the file is a recipe
   that cannot say what it cooks to. */
static bool
cooked_path( const char* root_abs_dir, const char* rel_dir, const char* disk_name, char* out, size_t cap, const char** why )
{
    const char* ext   = name_ext( disk_name );
    size_t      stem  = ( size_t )( ext - disk_name );          /* includes the '.' when there is one */
    res_kind_t  kind  = res_kind_from_ext( ext );
    const char* cext  = NULL;

    if ( kind == RES_KIND_RECIPE )
    {
        char abs[ RT_MAX_PATH ];
        snprintf( abs, sizeof( abs ), "%s/%s", root_abs_dir, disk_name );
        kind = recipe_kind( abs, why );
        if ( kind == RES_KIND_COPY )
            return false;
    }
    cext = res_kind_cooked_ext( kind );
    if ( !cext )
        cext = ext;                                             /* verbatim copy keeps its own */

    if ( *ext == 0 )
        snprintf( out, cap, "%s%s%s", rel_dir, *rel_dir ? "/" : "", disk_name );
    else
        snprintf( out, cap, "%s%s%.*s%s", rel_dir, *rel_dir ? "/" : "", ( int )stem, disk_name, cext );

    for ( char* p = out; *p; ++p )
        *p = res_canon_char( *p );
    return true;
}

typedef enum leaf_result_e
{
    LEAF_NONE,          // nothing of that name in this root
    LEAF_FOUND,         // path filled
    LEAF_AMBIGUOUS,     // two files claim the name; `first` and `second` name them
    LEAF_DIR_ONLY,      // only a directory of that name exists
    LEAF_BAD_RECIPE,    // the file is a recipe with no usable kind; `why` says what

} leaf_result_t;

typedef struct leaf_hit_s
{
    char        path[ RT_MAX_PATH ];   // cooked relative path
    const char* first;                 // on-disk names for an ambiguity report
    const char* second;
    const char* why;

} leaf_hit_t;

/* Resolve leaf `canon` ("a/b/c") within one root. */
static leaf_result_t
leaf_resolve( const char* root, const char* canon, leaf_hit_t* hit )
{
    const char* leaf = strrchr( canon, '/' );
    char        dir[ RES_NAME_MAX + 1 ];
    if ( leaf )
    {
        memcpy( dir, canon, ( size_t )( leaf - canon ) );
        dir[ leaf - canon ] = 0;
        leaf++;
    }
    else
    {
        dir[ 0 ] = 0;
        leaf     = canon;
    }

    char abs[ RT_MAX_PATH ], rel[ RT_MAX_PATH ];
    if ( !dir_resolve( root, dir, abs, sizeof( abs ), rel, sizeof( rel ) ) )
        return LEAF_NONE;

    /* Every file whose stem folds to the leaf.  Two of them is an ambiguity whatever their
       extensions: the name must stand for exactly one file. */
    const rt_dir_t*    d      = dir_list( abs );
    const rt_dirent_t* found  = NULL;
    const rt_dirent_t* second = NULL;
    size_t             n      = strlen( leaf );
    for ( int i = 0; i < d->count; ++i )
    {
        const rt_dirent_t* e = &d->ents[ i ];
        if ( e->is_dir )
            continue;
        const char* ext = name_ext( e->name );
        size_t stem = ( size_t )( ext - e->name );
        if ( *ext )
            stem--;                                             /* drop the '.' */
        if ( stem != n )
            continue;
        bool same = true;
        for ( size_t k = 0; k < n && same; ++k )
            same = ( res_canon_char( e->name[ k ] ) == leaf[ k ] );
        if ( !same )
            continue;
        if ( !found )
            found = e;
        else if ( !second )
            second = e;
    }

    if ( !found )
        return dir_find( d, leaf, n, true, NULL ) ? LEAF_DIR_ONLY : LEAF_NONE;
    if ( second )
    {
        hit->first  = found->name;
        hit->second = second->name;
        return LEAF_AMBIGUOUS;
    }
    if ( !cooked_path( abs, rel, found->name, hit->path, sizeof( hit->path ), &hit->why ) )
        return LEAF_BAD_RECIPE;
    return LEAF_FOUND;
}

/* Every file beneath `abs` (relative spelling `rel`, canonical prefix `canon_dir` with its
   trailing slash) in root `root` becomes an entry expanded from subtree `via`, unless the
   name is already present -- an explicit site, or an earlier root.  Recurses into
   subdirectories. */
static void
tree_expand( int via, int root, const char* abs, const char* rel, const char* canon_dir )
{
    const rt_dir_t* d = dir_list( abs );
    for ( int i = 0; i < d->count; ++i )
    {
        const rt_dirent_t* e = &d->ents[ i ];
        char canon[ RES_NAME_MAX + 1 ];
        char sub_abs[ RT_MAX_PATH ], sub_rel[ RT_MAX_PATH ];

        const char* ext  = name_ext( e->name );
        size_t      stem = e->is_dir ? strlen( e->name ) : ( size_t )( ext - e->name ) - ( *ext ? 1 : 0 );
        size_t      dl   = strlen( canon_dir );
        if ( dl + stem + 1 > RES_NAME_MAX )
        {
            resolve_error( &g_entries[ via ], "'%s%s' is longer than RES_NAME_MAX", canon_dir, e->name );
            continue;
        }
        memcpy( canon, canon_dir, dl );
        for ( size_t k = 0; k < stem; ++k )
            canon[ dl + k ] = res_canon_char( e->name[ k ] );
        canon[ dl + stem ] = 0;

        snprintf( sub_abs, sizeof( sub_abs ), "%s/%s", abs, e->name );
        snprintf( sub_rel, sizeof( sub_rel ), "%s%s%s", rel, *rel ? "/" : "", e->name );

        if ( e->is_dir )
        {
            canon[ dl + stem ]     = '/';
            canon[ dl + stem + 1 ] = 0;
            tree_expand( via, root, sub_abs, sub_rel, canon );
            continue;
        }

        /* Already named: by a RID site, by an earlier (shadowing) root, or -- when it came
           from this same subtree in this same root -- by a second file with the same stem
           in one directory, which the name cannot stand for. */
        int existing = entry_find( canon, res_hash_name( canon ) );
        if ( existing >= 0 )
        {
            if ( g_entries[ existing ].via == via && g_entries[ existing ].root == root )
                resolve_error( &g_entries[ via ], "'%s' is claimed by two files, '%s' and '%s'",
                               canon, path_basename( g_entries[ existing ].path ), e->name );
            continue;
        }

        const char* why = NULL;
        char        path[ RT_MAX_PATH ];
        if ( !cooked_path( abs, rel, e->name, path, sizeof( path ), &why ) )
        {
            resolve_error( &g_entries[ via ], "%s: %s", sub_rel, why );
            continue;
        }
        int idx = entry_push( canon, dl + stem, canon, false, g_entries[ via ].file, g_entries[ via ].line, via );
        free( g_entries[ idx ].path );
        g_entries[ idx ].path = rt_strdup( path, strlen( path ) );
        g_entries[ idx ].root = root;
    }
}

static void
resolve_all( void )
{
    int explicit_count = g_entry_count;    /* expansion appends; only the sites are walked */

    if ( g_root_count == 0 && explicit_count > 0 )
    {
        for ( int i = 0; i < explicit_count; ++i )
            resolve_error( &g_entries[ i ], "no content root was given (-root), so no name can resolve" );
        return;
    }

    /* Leaves first, each taking the first root that holds it. */
    for ( int i = 0; i < explicit_count; ++i )
    {
        rt_entry_t* e = &g_entries[ i ];
        if ( e->tree )
            continue;

        leaf_hit_t    hit    = { 0 };
        leaf_result_t result = LEAF_NONE;
        bool          dir_only = false;
        int           root     = -1;
        for ( int r = 0; r < g_root_count && result == LEAF_NONE; ++r )
        {
            result = leaf_resolve( g_roots[ r ], e->name, &hit );
            root   = r;
            if ( result == LEAF_DIR_ONLY )
            {
                dir_only = true;
                result   = LEAF_NONE;    /* a lower root may still hold the file */
            }
        }

        switch ( result )
        {
            case LEAF_FOUND:
                free( e->path );
                e->path = rt_strdup( hit.path, strlen( hit.path ) );
                e->root = root;
                break;
            case LEAF_AMBIGUOUS:
                resolve_error( e, "matches two files, '%s' and '%s' -- a name stands for exactly one",
                               hit.first, hit.second );
                break;
            case LEAF_BAD_RECIPE:
                resolve_error( e, "%s", hit.why );
                break;
            default:
                if ( dir_only )
                    resolve_error( e, "'%s' is a directory, not a file -- RES_TREE( \"%s\" ) names its contents",
                                   e->name, e->spelled );
                else
                    resolve_error( e, "no source file '%s.*' under any content root (%s%s)", e->name,
                                   g_roots[ 0 ], g_root_count > 1 ? ", ..." : "" );
                break;
        }
    }

    /* Subtrees: every root that holds the directory contributes, higher roots first, so a
       project file shadows the engine's under the same name. */
    for ( int i = 0; i < explicit_count; ++i )
    {
        rt_entry_t* e = &g_entries[ i ];
        if ( !e->tree )
            continue;

        char canon_dir[ RES_NAME_MAX + 1 ];
        snprintf( canon_dir, sizeof( canon_dir ), "%s", e->name );
        canon_dir[ strlen( canon_dir ) - 1 ] = 0;              /* strip the subtree slash */

        int found = 0;
        for ( int r = 0; r < g_root_count; ++r )
        {
            char abs[ RT_MAX_PATH ], rel[ RT_MAX_PATH ];
            if ( !dir_resolve( g_roots[ r ], canon_dir, abs, sizeof( abs ), rel, sizeof( rel ) ) )
                continue;
            found++;
            tree_expand( i, r, abs, rel, g_entries[ i ].name );    /* g_entries may move: index, not e */
        }
        if ( found == 0 )
            resolve_error( &g_entries[ i ], "no directory '%s' under any content root (%s%s)", canon_dir,
                           g_roots[ 0 ], g_root_count > 1 ? ", ..." : "" );
    }
}

/*==============================================================================================
    Collision check

    Sorted by id, so two names on one hash sit side by side.
==============================================================================================*/

static int
entry_cmp_id( const void* a, const void* b )
{
    const rt_entry_t* ea = ( const rt_entry_t* )a;
    const rt_entry_t* eb = ( const rt_entry_t* )b;
    if ( ea->id != eb->id )
        return ea->id < eb->id ? -1 : 1;
    return strcmp( ea->name, eb->name );
}

static void
check_collisions( void )
{
    if ( g_entry_count < 2 )
        return;

    /* Sorting the live array would break the `via` indices, so a copy is sorted. */
    rt_entry_t* sorted = ( rt_entry_t* )rt_xrealloc( NULL, ( size_t )g_entry_count * sizeof( rt_entry_t ) );
    memcpy( sorted, g_entries, ( size_t )g_entry_count * sizeof( rt_entry_t ) );
    qsort( sorted, ( size_t )g_entry_count, sizeof( rt_entry_t ), entry_cmp_id );

    for ( int i = 1; i < g_entry_count; ++i )
    {
        const rt_entry_t* a = &sorted[ i - 1 ];
        const rt_entry_t* b = &sorted[ i ];
        if ( a->id == b->id )
        {
            const rt_entry_t* sa = entry_site( a );
            const rt_entry_t* sb = entry_site( b );
            rt_error( g_files[ sb->file ].path, sb->line,
                      "rid collision 0x%08x: '%s' (here) vs '%s' (%s:%d) -- rename one of them",
                      ( unsigned )b->id, b->name, a->name, g_files[ sa->file ].path, sa->line );
        }
    }
    free( sorted );
}

/*==============================================================================================
    Output

    Entries are written in canonical-name order, which makes every subtree a contiguous run
    of the table and keeps the generated file byte-stable across runs.
==============================================================================================*/

static int
entry_cmp_name( const void* a, const void* b )
{
    return strcmp( ( ( const rt_entry_t* )a )->name, ( ( const rt_entry_t* )b )->name );
}

static bool
write_table( const char* out_path, const char* symbol )
{
    FILE* f = fopen( out_path, "wb" );
    if ( !f )
    {
        rt_error( NULL, 0, "cannot write '%s'", out_path );
        return false;
    }

    int expanded = 0;
    for ( int i = 0; i < g_entry_count; ++i )
        expanded += ( g_entries[ i ].via >= 0 );

    fprintf( f, "/*  %s_res_table.c -- generated by res_tool; do not edit.\n\n", symbol );
    fprintf( f, "    Every resource name this image references through RID() or RES_TREE(),\n" );
    fprintf( f, "    harvested from %d source file(s), with the cooked file each resolves to\n", g_file_count );
    fprintf( f, "    under the content root (%d found beneath declared subtrees).  A trailing\n", expanded );
    fprintf( f, "    slash marks a subtree, which has no file of its own.  Registered with the\n" );
    fprintf( f, "    resource catalogue when the image's module descriptor comes online\n" );
    fprintf( f, "    (MOD_RES_TABLE).  */\n\n" );
    fprintf( f, "#include \"engine/res/res.h\"\n\n" );

    if ( g_entry_count == 0 )
    {
        fprintf( f, "const res_table_t g_%s_res_table = { .entries = NULL, .count = 0 };\n", symbol );
        fclose( f );
        return true;
    }

    /* The site comments are written from a sorted copy; `via` still indexes g_entries. */
    rt_entry_t* sorted = ( rt_entry_t* )rt_xrealloc( NULL, ( size_t )g_entry_count * sizeof( rt_entry_t ) );
    memcpy( sorted, g_entries, ( size_t )g_entry_count * sizeof( rt_entry_t ) );
    qsort( sorted, ( size_t )g_entry_count, sizeof( rt_entry_t ), entry_cmp_name );

    /* Columns line up so the table reads as a list. */
    int name_w = 0, path_w = 0;
    for ( int i = 0; i < g_entry_count; ++i )
    {
        int w = ( int )strlen( sorted[ i ].name );
        if ( w > name_w ) name_w = w;
        w = ( int )strlen( sorted[ i ].path );
        if ( w > path_w ) path_w = w;
    }

    fprintf( f, "static const res_entry_t s_%s_res_entries[] = {\n", symbol );
    for ( int i = 0; i < g_entry_count; ++i )
    {
        const rt_entry_t* e    = &sorted[ i ];
        const rt_entry_t* site = entry_site( e );
        int               np   = name_w - ( int )strlen( e->name );
        int               pp   = path_w - ( int )strlen( e->path );
        fprintf( f, "    { \"%s\",%*s\"%s\" },%*s/* 0x%08x  ", e->name, np + 1, "", e->path, pp + 4, "",
                 ( unsigned )e->id );
        if ( e->via >= 0 )
            fprintf( f, "in %s  ", g_entries[ e->via ].name );
        fprintf( f, "%s:%d */\n", path_basename( g_files[ site->file ].path ), site->line );
    }
    fprintf( f, "};\n\n" );
    fprintf( f, "const res_table_t g_%s_res_table = {\n", symbol );
    fprintf( f, "    .entries = s_%s_res_entries,\n", symbol );
    fprintf( f, "    .count   = %d,\n", g_entry_count );
    fprintf( f, "};\n" );
    fclose( f );
    free( sorted );
    return true;
}

static bool
write_deps( const char* deps_path )
{
    FILE* f = fopen( deps_path, "wb" );
    if ( !f )
    {
        rt_error( NULL, 0, "cannot write '%s'", deps_path );
        return false;
    }
    for ( int i = 0; i < g_dep_count; ++i )
        fprintf( f, "%s\n", g_deps[ i ] );
    fclose( f );
    return true;
}

/*==============================================================================================
    Entry point
==============================================================================================*/

static int
usage( void )
{
    printf( "usage: " RT_TOOL " -list <units.txt> -out <table.c> -name <symbol> [-inc <dir>]... [-root <dir>]... [-deps <file>] [-silent]\n" );
    printf( "  -list   file naming one translation unit per line; '#' starts a comment\n" );
    printf( "  -out    generated C file defining g_<symbol>_res_table\n" );
    printf( "  -name   symbol base name (the target name, or 'host' for an executable)\n" );
    printf( "  -inc    quoted-include search root; repeatable, searched in order\n" );
    printf( "  -root   content root; repeatable, the first that holds a name wins\n" );
    printf( "  -deps   file to write the directories and recipes the table depends on\n" );
    printf( "  -silent suppress the summary line\n" );
    return 0;
}

int
main( int argc, char** argv )
{
    const char* list_path = NULL;
    const char* out_path  = NULL;
    const char* deps_path = NULL;
    const char* symbol    = NULL;
    bool        silent    = false;

    for ( int i = 1; i < argc; ++i )
    {
        if      ( strcmp( argv[ i ], "-list"   ) == 0 && arg_has_value( argc, argv, i ) ) list_path = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-out"    ) == 0 && arg_has_value( argc, argv, i ) ) out_path  = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-deps"   ) == 0 && arg_has_value( argc, argv, i ) ) deps_path = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-name"   ) == 0 && arg_has_value( argc, argv, i ) ) symbol    = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-inc"    ) == 0 && arg_has_value( argc, argv, i ) )
        {
            if ( g_inc_dir_count == RT_MAX_INC_DIRS )
            {
                fprintf( stderr, "[" RT_TOOL "] too many -inc roots (max %d)\n", RT_MAX_INC_DIRS );
                return 1;
            }
            char* norm = ( char* )rt_xrealloc( NULL, RT_MAX_PATH );
            path_normalize( argv[ ++i ], norm, RT_MAX_PATH );
            g_inc_dirs[ g_inc_dir_count++ ] = norm;
        }
        else if ( strcmp( argv[ i ], "-root"   ) == 0 && arg_has_value( argc, argv, i ) )
        {
            if ( g_root_count == RT_MAX_ROOTS )
            {
                fprintf( stderr, "[" RT_TOOL "] too many -root dirs (max %d)\n", RT_MAX_ROOTS );
                return 1;
            }
            char* norm = ( char* )rt_xrealloc( NULL, RT_MAX_PATH );
            path_normalize( argv[ ++i ], norm, RT_MAX_PATH );
            g_roots[ g_root_count++ ] = norm;
        }
        else if ( strcmp( argv[ i ], "-silent" ) == 0 ) silent = true;
        else if ( strcmp( argv[ i ], "-help" ) == 0 || strcmp( argv[ i ], "-h" ) == 0 ) return usage();
        else
        {
            fprintf( stderr, "[" RT_TOOL "] unknown argument '%s'\n", argv[ i ] );
            return 1;
        }
    }

    if ( !list_path || !out_path || !symbol )
    {
        fprintf( stderr, "[" RT_TOOL "] -list, -out and -name are required\n" );
        usage();
        return 1;
    }

    /* Seed the queue from the list file. */
    {
        size_t n    = 0;
        char*  text = file_read( list_path, &n );
        if ( !text )
        {
            fprintf( stderr, "[" RT_TOOL "] cannot read list '%s'\n", list_path );
            return 1;
        }
        char* p = text;
        while ( *p )
        {
            char* line = p;
            while ( *p && *p != '\n' )
                p++;
            if ( *p )
                *p++ = 0;
            char* end = line + strlen( line );
            while ( end > line && ( end[ -1 ] == '\r' || end[ -1 ] == ' ' || end[ -1 ] == '\t' ) )
                *--end = 0;
            while ( *line == ' ' || *line == '\t' )
                line++;
            if ( !*line || *line == '#' )
                continue;
            if ( !file_exists( line ) )
            {
                rt_error( NULL, 0, "unit not found: %s", line );
                continue;
            }
            file_add( line );
        }
        free( text );
    }

    /* Scan; the queue grows as includes are discovered. */
    for ( int i = 0; i < g_file_count; ++i )
        lex_file( i );

    /* Every root is a dependency even when no name reaches into it: a root that appears
       later must make the table stale. */
    for ( int r = 0; r < g_root_count; ++r )
        dir_list( g_roots[ r ] );

    resolve_all();
    check_collisions();

    if ( g_error_count )
    {
        fprintf( stderr, "[" RT_TOOL "] %s: %d error(s); %s not written\n", symbol, g_error_count, out_path );
        remove( out_path );
        return 1;
    }

    if ( !write_table( out_path, symbol ) )
        return 1;
    if ( deps_path && !write_deps( deps_path ) )
        return 1;

    if ( !silent )
    {
        int expanded = 0;
        for ( int i = 0; i < g_entry_count; ++i )
            expanded += ( g_entries[ i ].via >= 0 );
        printf( "[" RT_TOOL "] %s: %d name(s) from %d file(s), %d from subtrees, %d root(s)\n", symbol,
                g_entry_count, g_file_count, expanded, g_root_count );
    }
    return 0;
}

// clang-format on
/*============================================================================================*/
