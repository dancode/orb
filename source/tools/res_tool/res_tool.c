/*==============================================================================================

    res_tool.c - Resource reference harvester (build-time)

    Usage:
        res_tool -list <units.txt> -out <table.c> -name <symbol> [-inc <dir>]... [-silent]

    Reads a list of translation units (one path per line; '#' starts a comment), follows
    every #include "..." reachable from them -- unity fragments and headers alike -- and
    collects the names spelled through RID( "..." ) and RES_TREE( "..." ).  Writes them as
    a C table of canonical names:

        const res_table_t g_<symbol>_res_table;

    which the executable or DLL compiles in and hands to the resource catalogue through its
    module descriptor (MOD_RES_TABLE).  build_tool invokes this once per image, over the
    image's own units plus those of every statically linked dependency; see
    build_gen_res_table in build_tool_09_exec.c.

    A RES_TREE prefix is recorded with a trailing slash, exactly as the macro hashes it, so
    "ui/icon/" (the subtree) and "ui/icon" (a leaf) are two entries with two ids.

    The tool is also where the catalogue's uniqueness guarantee is proven: every harvested
    name is hashed exactly as the runtime does (res_hash_name, header-inline) and two
    different names landing on one rid_t fail the build, naming both sites.  A malformed
    name (empty segment, leading or trailing separator, whitespace, non-ASCII) and a RID()
    whose argument is not a string literal fail the same way.  Every error is reported
    before the tool exits non-zero, so one run shows the whole list.

    Include resolution tries the including file's directory first and then each -inc root,
    in order -- the same search the compiler performs for a quoted include.  Angle-bracket
    includes and unresolved paths are skipped: those are system and SDK headers, which
    cannot hold engine resource names.  Conditional compilation is not evaluated, so both
    arms of a platform #ifdef are scanned; an over-inclusion there is harmless.

    Standalone C11: reads files with stdio, links nothing.  It includes engine/res/res.h for
    the hash and the canonical fold only, so the ids it computes are the runtime's.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/res/res.h"

// clang-format off
/*==============================================================================================
    Limits
==============================================================================================*/

#define RT_MAX_PATH         1024
#define RT_MAX_INC_DIRS     16
#define RT_MAX_ERRORS       64
#define RT_TOOL             "res_tool"

/*==============================================================================================
    Scanned file
==============================================================================================*/

typedef struct rt_file_s
{
    char*    path;      // normalized path, as opened
    char*    key;       // dedupe key: normalized, case-folded on Windows

} rt_file_t;

typedef struct rt_entry_s
{
    char*    name;      // canonical name (res_canon_char applied); trailing '/' = subtree
    char*    spelled;   // name as written at the first site that mentioned it
    rid_t    id;        // res_hash_name( name )
    int      file;      // index into g_files of the first site
    int      line;      // line of the first site

} rt_entry_t;

static rt_file_t*  g_files;
static int         g_file_count;
static int         g_file_cap;

static rt_entry_t* g_entries;
static int         g_entry_count;
static int         g_entry_cap;

static const char* g_inc_dirs[ RT_MAX_INC_DIRS ];
static int         g_inc_dir_count;

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
        n--;    /* drop the separator, keep a bare "/" root */
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

    for ( int i = 0; i < g_entry_count; ++i )
        if ( strcmp( g_entries[ i ].name, canon ) == 0 )
            return;

    if ( g_entry_count == g_entry_cap )
    {
        g_entry_cap = g_entry_cap ? g_entry_cap * 2 : 64;
        g_entries   = ( rt_entry_t* )rt_xrealloc( g_entries, ( size_t )g_entry_cap * sizeof( rt_entry_t ) );
    }
    rt_entry_t* e = &g_entries[ g_entry_count++ ];
    e->name    = rt_strdup( canon, n );
    e->spelled = rt_strdup( spelled, strlen( spelled ) );
    e->id      = res_hash_name( canon );
    e->file    = file;
    e->line    = line;
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
    Collision check

    Sorted by id, so two names on one hash sit side by side.  The sort also fixes the output
    order, which keeps the generated file byte-stable across runs.
==============================================================================================*/

static int
entry_cmp( const void* a, const void* b )
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
    if ( g_entry_count > 1 )
        qsort( g_entries, ( size_t )g_entry_count, sizeof( rt_entry_t ), entry_cmp );

    for ( int i = 1; i < g_entry_count; ++i )
    {
        const rt_entry_t* a = &g_entries[ i - 1 ];
        const rt_entry_t* b = &g_entries[ i ];
        if ( a->id == b->id )
        {
            rt_error( g_files[ b->file ].path, b->line,
                      "rid collision 0x%08x: '%s' (here) vs '%s' (%s:%d) -- rename one of them",
                      ( unsigned )b->id, b->spelled, a->spelled,
                      g_files[ a->file ].path, a->line );
        }
    }
}

/*==============================================================================================
    Output
==============================================================================================*/

static bool
write_table( const char* out_path, const char* symbol )
{
    FILE* f = fopen( out_path, "wb" );
    if ( !f )
    {
        rt_error( NULL, 0, "cannot write '%s'", out_path );
        return false;
    }

    fprintf( f, "/*  %s_res_table.c -- generated by res_tool; do not edit.\n\n", symbol );
    fprintf( f, "    Every resource name this image references through RID() or RES_TREE(),\n" );
    fprintf( f, "    harvested from %d source file(s).  A trailing slash marks a subtree.\n", g_file_count );
    fprintf( f, "    Registered with the resource catalogue when the image's module descriptor\n" );
    fprintf( f, "    comes online (MOD_RES_TABLE).  */\n\n" );
    fprintf( f, "#include \"engine/res/res.h\"\n\n" );

    if ( g_entry_count == 0 )
    {
        fprintf( f, "const res_table_t g_%s_res_table = { .entries = NULL, .count = 0 };\n", symbol );
        fclose( f );
        return true;
    }

    /* Location comments line up in one column so the table reads as a list. */
    int width = 0;
    for ( int i = 0; i < g_entry_count; ++i )
    {
        int w = ( int )strlen( g_entries[ i ].name );
        if ( w > width )
            width = w;
    }

    fprintf( f, "static const res_entry_t s_%s_res_entries[] = {\n", symbol );
    for ( int i = 0; i < g_entry_count; ++i )
    {
        const rt_entry_t* e   = &g_entries[ i ];
        int               pad = width - ( int )strlen( e->name );
        fprintf( f, "    { \"%s\" },%*s/* 0x%08x  %s:%d */\n", e->name, pad + 4, "",
                 ( unsigned )e->id, path_basename( g_files[ e->file ].path ), e->line );
    }
    fprintf( f, "};\n\n" );
    fprintf( f, "const res_table_t g_%s_res_table = {\n", symbol );
    fprintf( f, "    .entries = s_%s_res_entries,\n", symbol );
    fprintf( f, "    .count   = %d,\n", g_entry_count );
    fprintf( f, "};\n" );
    fclose( f );
    return true;
}

/*==============================================================================================
    Entry point
==============================================================================================*/

static int
usage( void )
{
    printf( "usage: " RT_TOOL " -list <units.txt> -out <table.c> -name <symbol> [-inc <dir>]... [-silent]\n" );
    printf( "  -list   file naming one translation unit per line; '#' starts a comment\n" );
    printf( "  -out    generated C file defining g_<symbol>_res_table\n" );
    printf( "  -name   symbol base name (the target name, or 'host' for an executable)\n" );
    printf( "  -inc    quoted-include search root; repeatable, searched in order\n" );
    printf( "  -silent suppress the summary line\n" );
    return 0;
}

int
main( int argc, char** argv )
{
    const char* list_path = NULL;
    const char* out_path  = NULL;
    const char* symbol    = NULL;
    bool        silent    = false;

    for ( int i = 1; i < argc; ++i )
    {
        if      ( strcmp( argv[ i ], "-list"   ) == 0 && arg_has_value( argc, argv, i ) ) list_path = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-out"    ) == 0 && arg_has_value( argc, argv, i ) ) out_path  = argv[ ++i ];
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

    check_collisions();

    if ( g_error_count )
    {
        fprintf( stderr, "[" RT_TOOL "] %s: %d error(s); %s not written\n", symbol, g_error_count, out_path );
        remove( out_path );
        return 1;
    }

    if ( !write_table( out_path, symbol ) )
        return 1;

    if ( !silent )
        printf( "[" RT_TOOL "] %s: %d name(s) from %d file(s)\n", symbol, g_entry_count, g_file_count );
    return 0;
}

// clang-format on
/*============================================================================================*/
