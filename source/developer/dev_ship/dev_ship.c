/*==============================================================================================

    dev_ship.c -- Developer ship-pipeline library.

    See dev_ship.h for the pipeline contract.  Each stage is a static function behind the
    dev_ship_run_stage() dispatcher; dev_ship_run() drives all stages in order and stops at
    the first failure.  Errors land in a static message buffer (single-threaded tool code,
    same convention as dev_image).

    build shells out to build_tool.  verify and stage share one content-set walk: the ship
    image's resource manifests (the exe target's plus one per module it loads), closed over
    the reference section of every cooked file they name.  stage copies that set; package
    writes the file manifest; deploy publishes the staged tree.

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/pack/pack_host.h"    /* pack_crc32 for the package manifest */
#include "engine/res/res.h"           /* RES_NAME_MAX, res_name_ok */
#include "engine/res/res_ref.h"       /* res_ref_locate / res_ref_next over cooked files */
#include "engine/res/res_cook.h"      /* source extension -> cooked extension */
#include "developer/dev_ship/dev_ship.h"

/*==============================================================================================
    Platform path helpers
==============================================================================================*/

#if OS_WINDOWS
    #define PATH_SEP        "\\"
    #define BUILD_TOOL_EXE  "build_tool.exe"
#else
    #define PATH_SEP        "/"
    #define BUILD_TOOL_EXE  "build_tool"
#endif

#define SHIP_PATH_MAX   512
#define SHIP_NAME_MAX   64      /* a target name */
#define SHIP_MOD_MAX    16      /* modules one ship image loads */

/*==============================================================================================
    Error reporting
==============================================================================================*/

static char s_error[ 512 ];

static void
ship_set_error( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( s_error, sizeof( s_error ), fmt, args );
    va_end( args );
}

const char*
dev_ship_last_error( void )
{
    return s_error;
}

/*==============================================================================================
    Stage names
==============================================================================================*/

static const char* s_stage_names[ DEV_SHIP_STAGE_COUNT ] =
{
    [ DEV_SHIP_BUILD ]   = "build",
    [ DEV_SHIP_VERIFY ]  = "verify",
    [ DEV_SHIP_STAGE ]   = "stage",
    [ DEV_SHIP_PACKAGE ] = "package",
    [ DEV_SHIP_DEPLOY ]  = "deploy",
};

const char*
dev_ship_stage_name( dev_ship_stage_t stage )
{
    if ( stage < 0 || stage >= DEV_SHIP_STAGE_COUNT )
        return NULL;
    return s_stage_names[ stage ];
}

/*==============================================================================================
    Path helpers
==============================================================================================*/

/* Effective staging root: desc->out_dir, or the <root>/build/ship/<project> default. */
static void
ship_out_dir( const dev_ship_desc_t* desc, char* out, int size )
{
    if ( desc->out_dir && desc->out_dir[ 0 ] )
        snprintf( out, (size_t)size, "%s", desc->out_dir );
    else
        snprintf( out, (size_t)size, "%s" PATH_SEP "build" PATH_SEP "ship" PATH_SEP "%s",
                  desc->root_dir, desc->project );
}

/* Engine root: supplies build_tool, and (with-engine) the host exe, module DLLs and content.
   Returns desc->root_dir when engine_dir is unset -- an engine-resident target, where the two
   roots coincide.  A child project sets engine_dir (from .orb_engine), so the returned pointer
   differs from root_dir: that inequality is exactly the "is this a child project?" test. */
static const char*
ship_engine_root( const dev_ship_desc_t* desc )
{
    return ( desc->engine_dir && desc->engine_dir[ 0 ] ) ? desc->engine_dir : desc->root_dir;
}

/* Copy one file, creating the destination's parent directories first. */
static bool
ship_copy_file( const char* src, const char* dst )
{
    char parent[ SHIP_PATH_MAX ];
    snprintf( parent, sizeof( parent ), "%s", dst );
    for ( char* p = parent + strlen( parent ); p > parent; --p )
        if ( p[ -1 ] == '/' || p[ -1 ] == '\\' )
        {
            p[ -1 ] = '\0';
            sys_dir_make( parent );
            break;
        }

    if ( !sys_file_copy( src, dst ) )
    {
        ship_set_error( "copy failed: %s -> %s", src, dst );
        return false;
    }
    return true;
}

/* Recursive tree copy via sys_dir_walk.  The walk joins each entry onto the root string it
   was given, so the relative remainder is full_path + strlen(src) and the destination is
   dst + that remainder. */
typedef struct
{
    const char* src;
    const char* dst;
    int         copied;
    bool        ok;

} ship_copy_ctx_t;

static bool
ship_copy_tree_cb( const char* filename, const char* full_path, void* userdata )
{
    (void)filename;
    ship_copy_ctx_t* ctx = userdata;

    char dst[ SHIP_PATH_MAX ];
    snprintf( dst, sizeof( dst ), "%s%s", ctx->dst, full_path + strlen( ctx->src ) );

    if ( !ship_copy_file( full_path, dst ) )
    {
        ctx->ok = false;
        return false;    /* stop the walk; the error is already set */
    }
    ++ctx->copied;
    return true;
}

/* Copy every file under src into dst, preserving the subtree.  Returns false only on a
   failed copy; a missing/empty source is 0 copies, not an error. */
static bool
ship_copy_tree( const char* src, const char* dst, int* copied )
{
    ship_copy_ctx_t ctx = { .src = src, .dst = dst, .copied = 0, .ok = true };
    sys_dir_walk( src, ship_copy_tree_cb, &ctx );
    *copied = ctx.copied;
    return ctx.ok;
}

/* Delete every file under root.  Directory skeletons are left in place (sys_dir_delete is
   single-level and the walk reports files only) -- staging overwrites into the same layout,
   so stale FILES are the hazard, empty dirs are cosmetic. */
static bool
ship_delete_tree_files( const char* filename, const char* full_path, void* userdata )
{
    (void)filename;
    int* deleted = userdata;
    if ( sys_file_delete( full_path ) )
        ++*deleted;
    return true;
}

/*==============================================================================================
    Validation -- shared by every stage so each is independently callable.
==============================================================================================*/

static bool
ship_validate( const dev_ship_desc_t* desc )
{
    if ( !desc || !desc->project || !desc->project[ 0 ] )
    {
        ship_set_error( "no project name" );
        return false;
    }
    if ( !desc->config || !desc->config[ 0 ] )
    {
        ship_set_error( "no build config" );
        return false;
    }
    if ( !desc->root_dir || !desc->root_dir[ 0 ] )
    {
        ship_set_error( "no engine root" );
        return false;
    }

    /* The engine root is the directory holding orb.targets -- catches running from the
       wrong CWD before a stage half-completes against bad paths. */
    char probe[ SHIP_PATH_MAX ];
    snprintf( probe, sizeof( probe ), "%s" PATH_SEP "orb.targets", desc->root_dir );
    if ( !sys_file_exists( probe ) )
    {
        ship_set_error( "'%s' is not the engine root (no orb.targets)", desc->root_dir );
        return false;
    }
    return true;
}

/*==============================================================================================
    The ship image -- which exe target anchors the set and which modules it loads.

    orb.targets already says what a ship exe loads: the mono_dep list of its target is the
    module set linked in under -monolithic and loaded as DLLs otherwise.  The list is read
    from the file directly (build_tool owns the full grammar; a 'mono_dep' line is a name
    list, '#' starts a comment) rather than mirrored in code.
==============================================================================================*/

typedef struct
{
    char exe[ SHIP_NAME_MAX ];                    /* exe target whose manifest anchors the set */
    char mods[ SHIP_MOD_MAX ][ SHIP_NAME_MAX ];   /* module targets the image loads */
    int  mod_count;

} ship_images_t;

static void
ship_images_add( ship_images_t* img, const char* name )
{
    for ( int i = 0; i < img->mod_count; ++i )
        if ( strcmp( img->mods[ i ], name ) == 0 )
            return;
    if ( img->mod_count < SHIP_MOD_MAX )
        snprintf( img->mods[ img->mod_count++ ], SHIP_NAME_MAX, "%s", name );
}

/* Append the mono_dep names of `target` in the orb.targets at `path`.  Returns the number of
   names read, or -1 when the file has no such target block. */
static int
ship_mono_deps( const char* path, const char* target, ship_images_t* img )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return -1;

    bool found    = false;
    bool in_block = false;
    int  added    = 0;
    char line[ 1024 ];
    while ( fgets( line, sizeof( line ), f ) )
    {
        char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;
        if ( *p == '#' || *p == '\0' || *p == '\r' || *p == '\n' )
            continue;

        char* key = p;
        while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) ++p;
        if ( *p ) *p++ = '\0';
        while ( *p == ' ' || *p == '\t' ) ++p;

        if ( strcmp( key, "target" ) == 0 )
        {
            char* name = p;
            while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#' ) ++p;
            *p = '\0';
            in_block = ( strcmp( name, target ) == 0 );
            found    = found || in_block;
            continue;
        }
        if ( !in_block || strcmp( key, "mono_dep" ) != 0 )
            continue;

        while ( *p && *p != '#' && *p != '\r' && *p != '\n' )
        {
            char* tok = p;
            while ( *p && *p != ' ' && *p != '\t' && *p != '#' && *p != '\r' && *p != '\n' ) ++p;
            char saved = *p;
            *p = '\0';
            if ( *tok )
            {
                ship_images_add( img, tok );
                ++added;
            }
            if ( saved == '\0' || saved == '#' || saved == '\r' || saved == '\n' )
                break;
            ++p;
            while ( *p == ' ' || *p == '\t' ) ++p;
        }
    }
    fclose( f );
    return found ? added : -1;
}

/* Resolve the image for desc: the exe target and the modules it loads.

   -target:      the named exe target itself; modules are its mono_dep list.
   project:      the <project>_ship target (monolithic) or host_game (modular); modules are
                 <project>_ship's mono_dep list -- render, game and the project -- falling back
                 to host_game's list plus the project when the project has no ship target (a
                 child project). */
static bool
ship_images( const dev_ship_desc_t* desc, ship_images_t* img )
{
    memset( img, 0, sizeof( *img ) );

    char targets[ SHIP_PATH_MAX ];
    snprintf( targets, sizeof( targets ), "%s" PATH_SEP "orb.targets", desc->root_dir );

    if ( desc->flags & DEV_SHIP_TARGET )
    {
        snprintf( img->exe, sizeof( img->exe ), "%s", desc->project );
        if ( ship_mono_deps( targets, img->exe, img ) < 0 )
        {
            ship_set_error( "no target '%s' in %s", img->exe, targets );
            return false;
        }
        return true;
    }

    char ship_target[ SHIP_NAME_MAX ];
    snprintf( ship_target, sizeof( ship_target ), "%s_ship", desc->project );
    int n = ship_mono_deps( targets, ship_target, img );

    if ( desc->flags & DEV_SHIP_MODULAR )
    {
        snprintf( img->exe, sizeof( img->exe ), "host_game" );
        if ( n < 0 )
        {
            snprintf( targets, sizeof( targets ), "%s" PATH_SEP "orb.targets", ship_engine_root( desc ) );
            if ( ship_mono_deps( targets, "host_game", img ) < 0 )
            {
                ship_set_error( "no 'host_game' target in %s", targets );
                return false;
            }
        }
        ship_images_add( img, desc->project );
        return true;
    }

    snprintf( img->exe, sizeof( img->exe ), "%s", ship_target );
    if ( n < 0 )
    {
        ship_set_error( "monolithic ship needs a '%s' target in %s (see the PATTERN block); "
                        "use -modular for a project without one", ship_target, targets );
        return false;
    }
    return true;
}

/* Where a module's binaries and build outputs live: the project's own tree for the project
   DLL, the engine tree for everything else. */
static const char*
ship_module_root( const dev_ship_desc_t* desc, const char* module )
{
    return strcmp( module, desc->project ) == 0 ? desc->root_dir : ship_engine_root( desc );
}

/*==============================================================================================
    The content set -- every file the image reads, by resource name.

    A name resolves the way res_tool resolved it at build time: under the first content root
    that holds a file with that stem (the project's content/ shadows the engine's), and a
    stem must match exactly one file there.  A source that cooks (a stage-tagged .hlsl, a
    .recipe) is represented by its cooked file under build/content, which is what the runtime
    reads; loose content (images, text) is the source file itself.  Every cooked file opens
    with a reference section (res_ref.h) naming what it needs in turn; the walk follows those
    names until the set closes.
==============================================================================================*/

typedef struct
{
    char name[ RES_NAME_MAX + 1 ];   /* resource name */
    char src[ SHIP_PATH_MAX ];       /* the file the runtime reads: cooked form or loose source */
    char ext[ 32 ];                  /* extension the staged file carries, no dot */
    bool cooked;

} ship_res_t;

typedef struct
{
    const dev_ship_desc_t* desc;
    char        roots[ 2 ][ SHIP_PATH_MAX ];    /* content roots, highest priority first */
    char        cooked[ 2 ][ SHIP_PATH_MAX ];   /* cooked mirrors, same order */
    int         root_count;
    ship_res_t* items;
    int         count;
    int         cap;
    int         cooked_count;
    int         manifests;                      /* manifests folded in */

} ship_set_t;

static void
ship_set_init( const dev_ship_desc_t* desc, ship_set_t* set )
{
    memset( set, 0, sizeof( *set ) );
    set->desc = desc;

    const char* roots[ 2 ] = { desc->root_dir, ship_engine_root( desc ) };
    int         n          = ( roots[ 1 ] != roots[ 0 ] ) ? 2 : 1;
    for ( int i = 0; i < n; ++i )
    {
        snprintf( set->roots[ i ],  SHIP_PATH_MAX, "%s" PATH_SEP "content", roots[ i ] );
        snprintf( set->cooked[ i ], SHIP_PATH_MAX, "%s" PATH_SEP "build" PATH_SEP "content", roots[ i ] );
    }
    set->root_count = n;
}

static void
ship_set_free( ship_set_t* set )
{
    free( set->items );
    set->items = NULL;
    set->count = set->cap = 0;
}

static int
ship_set_find( const ship_set_t* set, const char* name )
{
    for ( int i = 0; i < set->count; ++i )
        if ( strcmp( set->items[ i ].name, name ) == 0 )
            return i;
    return -1;
}

static ship_res_t*
ship_set_push( ship_set_t* set )
{
    if ( set->count == set->cap )
    {
        int         cap   = set->cap ? set->cap * 2 : 64;
        ship_res_t* items = realloc( set->items, (size_t)cap * sizeof( *items ) );
        if ( !items )
            return NULL;
        set->items = items;
        set->cap   = cap;
    }
    ship_res_t* r = &set->items[ set->count++ ];
    memset( r, 0, sizeof( *r ) );
    return r;
}

/* sys_file_glob callback for ship_source_find: "<base>.*" also matches "<base>.a.b", so the
   stem (everything before the LAST dot) is compared exactly. */
typedef struct
{
    const char* base;
    int         hits;
    char        first[ 128 ];
    char        second[ 128 ];

} ship_glob_ctx_t;

static bool
ship_source_glob_cb( const char* filename, const char* full_path, void* userdata )
{
    (void)full_path;
    ship_glob_ctx_t* c   = userdata;
    const char*      dot = strrchr( filename, '.' );
    if ( !dot || dot == filename )
        return true;
    if ( (size_t)( dot - filename ) != strlen( c->base ) || strncmp( filename, c->base, strlen( c->base ) ) != 0 )
        return true;
    if ( c->hits == 0 )      snprintf( c->first,  sizeof( c->first ),  "%s", filename );
    else if ( c->hits == 1 ) snprintf( c->second, sizeof( c->second ), "%s", filename );
    ++c->hits;
    return true;
}

/* Find the source file for `name`: the first root holding exactly one "<name>.<ext>".
   Returns 1 found (src/ext filled), 0 none, -1 ambiguous (error set). */
static int
ship_source_find( const ship_set_t* set, const char* name, char* src, int src_size,
                  char* ext, int ext_size )
{
    const char* slash = strrchr( name, '/' );
    const char* base  = slash ? slash + 1 : name;
    int         dirlen = slash ? (int)( slash - name ) : 0;

    char pattern[ 300 ];
    snprintf( pattern, sizeof( pattern ), "%s.*", base );

    for ( int r = 0; r < set->root_count; ++r )
    {
        char dir[ SHIP_PATH_MAX ];
        if ( dirlen )
            snprintf( dir, sizeof( dir ), "%s" PATH_SEP "%.*s", set->roots[ r ], dirlen, name );
        else
            snprintf( dir, sizeof( dir ), "%s", set->roots[ r ] );
        for ( char* c = dir; *c; ++c )
            if ( *c == '/' ) *c = PATH_SEP[ 0 ];

        ship_glob_ctx_t c = { .base = base };
        sys_file_glob( dir, pattern, ship_source_glob_cb, &c );
        if ( c.hits > 1 )
        {
            ship_set_error( "'%s' matches two files, '%s' and '%s' -- a name stands for exactly one",
                            name, c.first, c.second );
            return -1;
        }
        if ( c.hits == 1 )
        {
            snprintf( src, (size_t)src_size, "%s" PATH_SEP "%s", dir, c.first );
            snprintf( ext, (size_t)ext_size, "%s", strrchr( c.first, '.' ) + 1 );
            return 1;
        }
    }
    return 0;
}

/* The cooked extension (no dot) a source produces, or "" for loose content.  Mirrors the
   asset_tool manifest cook: a .hlsl cooks to .oshd, a .recipe to what its "kind" line says,
   and everything else -- images included -- is read by the runtime as it is. */
static bool
ship_cooked_ext( const char* src, const char* src_ext, char* out, int out_size )
{
    out[ 0 ] = '\0';
    if ( res_ext_is( src_ext, "hlsl" ) )
    {
        snprintf( out, (size_t)out_size, "%s", res_kind_cooked_ext( RES_KIND_SHADER ) );
        return true;
    }
    if ( !res_ext_is( src_ext, RES_RECIPE_EXT ) )
        return true;

    FILE* f = fopen( src, "rb" );
    if ( !f )
    {
        ship_set_error( "cannot read recipe '%s'", src );
        return false;
    }
    char line[ 512 ];
    while ( fgets( line, sizeof( line ), f ) )
    {
        const char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;
        if ( strncmp( p, "kind", 4 ) != 0 || ( p[ 4 ] != ' ' && p[ 4 ] != '\t' ) )
            continue;
        p += 4;
        while ( *p == ' ' || *p == '\t' ) ++p;
        char word[ 32 ];
        int  n = 0;
        while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < (int)sizeof( word ) - 1 )
            word[ n++ ] = *p++;
        word[ n ] = '\0';
        const char* ext = res_kind_cooked_ext( res_kind_from_name( word ) );
        if ( ext )
            snprintf( out, (size_t)out_size, "%s", ext );
        break;
    }
    fclose( f );

    if ( !out[ 0 ] )
    {
        ship_set_error( "recipe '%s' names no cooking kind", src );
        return false;
    }
    return true;
}

/* Add `name` (and, transitively, everything its cooked file references) to the set.  `via`
   names what asked for it, for the error message. */
static bool
ship_set_add( ship_set_t* set, const char* name, const char* via )
{
    if ( ship_set_find( set, name ) >= 0 )
        return true;
    if ( !res_name_ok( name ) )
    {
        ship_set_error( "'%s' (named by %s) is not a canonical resource name", name, via );
        return false;
    }

    char src[ SHIP_PATH_MAX ], ext[ 32 ], cooked_ext[ 32 ];
    int  found = ship_source_find( set, name, src, sizeof( src ), ext, sizeof( ext ) );
    if ( found < 0 )
        return false;
    if ( found == 0 )
    {
        ship_set_error( "'%s' (named by %s) has no source file '%s.*' under any content root",
                        name, via, name );
        return false;
    }
    if ( !ship_cooked_ext( src, ext, cooked_ext, sizeof( cooked_ext ) ) )
        return false;

    ship_res_t* r = ship_set_push( set );
    if ( !r )
    {
        ship_set_error( "out of memory" );
        return false;
    }
    snprintf( r->name, sizeof( r->name ), "%s", name );

    if ( !cooked_ext[ 0 ] )
    {
        snprintf( r->src, sizeof( r->src ), "%s", src );
        snprintf( r->ext, sizeof( r->ext ), "%s", ext );
        return true;
    }

    /* Cooked: the runtime reads build/content/<name>.<cooked ext>, under whichever tree
       built it (the project's for its own images, the engine's for engine modules). */
    r->cooked = true;
    snprintf( r->ext, sizeof( r->ext ), "%s", cooked_ext );
    for ( int c = 0; c < set->root_count && !r->src[ 0 ]; ++c )
    {
        char cand[ SHIP_PATH_MAX ];
        snprintf( cand, sizeof( cand ), "%s" PATH_SEP "%s.%s", set->cooked[ c ], name, cooked_ext );
        for ( char* p = cand; *p; ++p )
            if ( *p == '/' ) *p = PATH_SEP[ 0 ];
        if ( sys_file_exists( cand ) )
            snprintf( r->src, sizeof( r->src ), "%s", cand );
    }
    if ( !r->src[ 0 ] )
    {
        ship_set_error( "'%s' (named by %s) is not cooked: no build/content/%s.%s -- run the build stage",
                        name, via, name, cooked_ext );
        return false;
    }
    ++set->cooked_count;

    /* Follow the cooked file's references.  The items array may grow during the recursion,
       so nothing from `r` is used past this point. */
    char cooked_path[ SHIP_PATH_MAX ];
    snprintf( cooked_path, sizeof( cooked_path ), "%s", r->src );

    sys_file_data_t fd = sys_file_read_entire( cooked_path );
    if ( !fd.ok )
    {
        ship_set_error( "cannot read '%s'", cooked_path );
        return false;
    }
    const u8* sec;
    u32       size, count;
    bool      ok = res_ref_locate( fd.data, fd.size, &sec, &size, &count );
    if ( !ok )
        ship_set_error( "'%s' has no valid reference section (recook it)", cooked_path );

    u32 cursor = 0;
    for ( u32 i = 0; ok && i < count; ++i )
    {
        const char* ref = res_ref_next( sec, size, &cursor );
        if ( !ref )
            break;
        char ref_name[ RES_NAME_MAX + 1 ];
        snprintf( ref_name, sizeof( ref_name ), "%s", ref );
        ok = ship_set_add( set, ref_name, name );
    }
    sys_file_free( &fd );
    return ok;
}

/* Fold in one image's manifest: <root>/build/obj/<target>/<target>_res_manifest.txt, one
   name per non-comment row (first column; a row whose name ends in '/' is a subtree marker
   whose leaves follow as rows of their own). */
static bool
ship_set_add_manifest( ship_set_t* set, const char* root, const char* target )
{
    char path[ SHIP_PATH_MAX ];
    snprintf( path, sizeof( path ),
              "%s" PATH_SEP "build" PATH_SEP "obj" PATH_SEP "%s" PATH_SEP "%s_res_manifest.txt",
              root, target, target );

    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
    {
        ship_set_error( "'%s' has no resource manifest (%s) -- build it first", target, path );
        return false;
    }

    bool  ok  = true;
    char* cur = (char*)fd.data;    /* NUL-terminated by sys_file_read_entire */
    while ( ok && *cur )
    {
        char* line = cur;
        char* nl   = strchr( line, '\n' );
        if ( nl ) *nl = '\0';
        cur = nl ? nl + 1 : line + strlen( line );

        while ( *line == ' ' || *line == '\t' ) ++line;
        if ( *line == '#' || *line == '\0' || *line == '\r' )
            continue;
        char* end = line;
        while ( *end && *end != ' ' && *end != '\t' && *end != '\r' ) ++end;
        *end = '\0';
        if ( end > line && end[ -1 ] == '/' )
            continue;

        ok = ship_set_add( set, line, target );
    }
    sys_file_free( &fd );
    if ( ok )
        ++set->manifests;
    return ok;
}

/* Build the whole set for desc's image. */
static bool
ship_set_build( const dev_ship_desc_t* desc, const ship_images_t* img, ship_set_t* set )
{
    ship_set_init( desc, set );

    /* The exe's manifest comes from the tree that built the exe: the engine's for a modular
       ship (host_game) and for an engine-resident target, the project's for its own ship
       target. */
    const char* exe_root = ( desc->flags & DEV_SHIP_MODULAR ) ? ship_engine_root( desc ) : desc->root_dir;
    if ( !ship_set_add_manifest( set, exe_root, img->exe ) )
        return false;
    for ( int i = 0; i < img->mod_count; ++i )
        if ( !ship_set_add_manifest( set, ship_module_root( desc, img->mods[ i ] ), img->mods[ i ] ) )
            return false;
    return true;
}

/*==============================================================================================
    build -- compile the ship exe set via build_tool.

    Default (monolithic): the <project>_ship target (see the PATTERN block in orb.targets) --
    one generic host source with the project baked in, built -monolithic so modules archive
    as static libs and the result is a single exe.  Mode switches are safe: build_tool's
    per-(config,mode) stamps rebuild anything the previous mode left stale.

    DEV_SHIP_MODULAR: host_game.exe plus the module DLL set, each its own build_tool run
    (-target restricts to one target's closure, and the DLLs are not in the host's closure).

    DEV_SHIP_TARGET: the named exe target itself, -monolithic by default; modular builds it
    plus its mono_dep modules.
==============================================================================================*/

/* Human label for the ship shape -- used in build logs and the manifest header. */
static const char*
ship_shape_name( const dev_ship_desc_t* desc )
{
    if ( desc->flags & DEV_SHIP_NO_ENGINE ) return "project-only";
    if ( desc->flags & DEV_SHIP_MODULAR )   return "modular";
    return "monolithic";
}

static bool
ship_build_target( const dev_ship_desc_t* desc, const char* target, bool monolithic )
{
    /* build_tool.exe lives in the ENGINE bin (a child project's bin holds only a forwarder bat).
       Run it with the project root as the working dir so it reads that project's orb.targets. */
    const char* eng = ship_engine_root( desc );

    char cmd[ 1024 ];
    snprintf( cmd, sizeof( cmd ),
              "\"%s" PATH_SEP "bin" PATH_SEP BUILD_TOOL_EXE "\"%s -config %s -target %s",
              eng, monolithic ? " -monolithic" : "", desc->config, target );

    printf( "[build]   %s\n", cmd );

    sys_process_result_t result;
    if ( !sys_process_run( cmd, desc->root_dir, &result ) )
    {
        ship_set_error( "could not launch build_tool: %s", cmd );
        return false;
    }
    if ( result.exit_code != 0 )
    {
        ship_set_error( "build_tool failed on '%s' (exit %d)", target, result.exit_code );
        return false;
    }
    return true;
}

static bool
ship_build( const dev_ship_desc_t* desc )
{
    if ( desc->flags & DEV_SHIP_SKIP_BUILD )
    {
        printf( "[build]   skipped (-skip-build): staging prebuilt bin output\n" );
        return true;
    }

    if ( desc->flags & DEV_SHIP_NO_ENGINE )
    {
        /* Light shape: just the project's own game DLL -- no host, no engine modules. */
        if ( !ship_build_target( desc, desc->project, false ) )
            return false;
        printf( "[build]   %s %s (project-only) built\n", desc->project, desc->config );
        return true;
    }

    /* With-engine child ship: the engine is a prebuilt "known quantity" -- its host exe and
       module DLLs already sit in the engine bin, so only the project's own DLL is ours to
       build.  Monolithic needs a <project>_ship target the child does not have, so require
       modular (host_game + engine DLLs + <project>.dll copied from their respective bins). */
    if ( ship_engine_root( desc ) != desc->root_dir && !( desc->flags & DEV_SHIP_TARGET ) )
    {
        if ( !( desc->flags & DEV_SHIP_MODULAR ) )
        {
            ship_set_error( "monolithic ship of a child project needs a '%s_ship' target; "
                            "use -modular (host_game + engine DLLs + %s.dll)",
                            desc->project, desc->project );
            return false;
        }
        if ( !ship_build_target( desc, desc->project, false ) )
            return false;
        printf( "[build]   %s %s (with engine, modular) built\n", desc->project, desc->config );
        return true;
    }

    ship_images_t img;
    if ( !ship_images( desc, &img ) )
        return false;

    if ( desc->flags & DEV_SHIP_MODULAR )
    {
        if ( !ship_build_target( desc, img.exe, false ) )
            return false;
        for ( int i = 0; i < img.mod_count; ++i )
            if ( !ship_build_target( desc, img.mods[ i ], false ) )
                return false;
    }
    else
    {
        if ( !ship_build_target( desc, img.exe, true ) )
            return false;
    }

    printf( "[build]   %s %s (%s) built\n", desc->project, desc->config, ship_shape_name( desc ) );
    return true;
}

/*==============================================================================================
    verify -- check the content set on demand.

    Resolves the image's whole set (every manifest name and every cooked-file reference to
    exactly one file, cooked where it must be), then walks the complete content roots for
    two things the resolution alone cannot see: a stem claimed by two files in one directory
    (a name can stand for only one), and files no image of this ship references -- reported,
    not failed: they simply do not ship.
==============================================================================================*/

typedef struct
{
    const ship_set_t* set;
    const char*       root;       /* content root being walked */
    char            (*stems)[ RES_NAME_MAX + 1 ];
    char            (*files)[ SHIP_PATH_MAX ];
    int               count;
    int               cap;
    int               collisions;
    int               orphans;

} ship_verify_ctx_t;

static bool
ship_verify_walk_cb( const char* filename, const char* full_path, void* userdata )
{
    (void)filename;
    ship_verify_ctx_t* ctx = userdata;

    /* Root-relative path with forward slashes; the stem is everything before the last dot
       of the file name. */
    const char* tail = full_path + strlen( ctx->root );
    while ( *tail == '/' || *tail == '\\' ) ++tail;

    char rel[ SHIP_PATH_MAX ];
    snprintf( rel, sizeof( rel ), "%s", tail );
    for ( char* c = rel; *c; ++c )
        if ( *c == '\\' ) *c = '/';

    char stem[ RES_NAME_MAX + 1 ];
    snprintf( stem, sizeof( stem ), "%s", rel );
    char* base = strrchr( stem, '/' );
    char* dot  = strrchr( base ? base : stem, '.' );
    if ( dot && dot != ( base ? base + 1 : stem ) )
        *dot = '\0';

    for ( int i = 0; i < ctx->count; ++i )
        if ( strcmp( ctx->stems[ i ], stem ) == 0 )
        {
            printf( "[verify]  collision: '%s' is claimed by '%s' and '%s'\n", stem, ctx->files[ i ], rel );
            ++ctx->collisions;
            return true;
        }

    if ( ctx->count == ctx->cap )
    {
        int cap = ctx->cap ? ctx->cap * 2 : 256;
        void* s = realloc( ctx->stems, (size_t)cap * sizeof( *ctx->stems ) );
        void* f = realloc( ctx->files, (size_t)cap * sizeof( *ctx->files ) );
        if ( !s || !f )
            return false;
        ctx->stems = s;
        ctx->files = f;
        ctx->cap   = cap;
    }
    snprintf( ctx->stems[ ctx->count ], RES_NAME_MAX + 1, "%s", stem );
    snprintf( ctx->files[ ctx->count ], SHIP_PATH_MAX, "%s", rel );
    ++ctx->count;

    if ( ship_set_find( ctx->set, stem ) < 0 )
    {
        printf( "[verify]  unreferenced: %s\n", rel );
        ++ctx->orphans;
    }
    return true;
}

static bool
ship_verify( const dev_ship_desc_t* desc )
{
    if ( desc->flags & DEV_SHIP_NO_ENGINE )
    {
        printf( "[verify]  project-only: no content ships\n" );
        return true;
    }

    ship_images_t img;
    if ( !ship_images( desc, &img ) )
        return false;

    ship_set_t set;
    if ( !ship_set_build( desc, &img, &set ) )
    {
        ship_set_free( &set );
        return false;
    }

    printf( "[verify]  %s + %d module%s: %d file%s referenced, %d cooked\n",
            img.exe, img.mod_count, img.mod_count == 1 ? "" : "s",
            set.count, set.count == 1 ? "" : "s", set.cooked_count );

    int collisions = 0, orphans = 0;
    for ( int r = 0; r < set.root_count; ++r )
    {
        ship_verify_ctx_t ctx = { .set = &set, .root = set.roots[ r ] };
        sys_dir_walk( set.roots[ r ], ship_verify_walk_cb, &ctx );
        collisions += ctx.collisions;
        orphans    += ctx.orphans;
        free( ctx.stems );
        free( ctx.files );
    }
    ship_set_free( &set );

    printf( "[verify]  %d collision%s, %d unreferenced file%s\n",
            collisions, collisions == 1 ? "" : "s", orphans, orphans == 1 ? "" : "s" );
    if ( collisions )
    {
        ship_set_error( "%d name collision%s under content/ (see above)", collisions, collisions == 1 ? "" : "s" );
        return false;
    }
    return true;
}

/*==============================================================================================
    stage -- gather the runtime file set into the staging directory.

    The staged layout mirrors the dev tree (see dev_ship.h): exes in <out>/bin because
    sys_root_dir() resolves the content root one level above the executable, so <out>/content
    is found with zero path changes.  Monolithic ships one renamed exe; modular ships
    host_game.exe + the module DLLs with their build names (kept so the DLL loader and .pdb
    references resolve) plus a launcher .bat carrying the -module argument.
==============================================================================================*/

/* Copy <src_root>/bin/<name> to <out>/bin/<dst_name> (NULL = keep the name); with DEV_SHIP_PDB
   also copy the like-named .pdb, under its BUILD name so the exe's embedded reference still
   resolves.  `required` distinguishes "not built" (error) from optional extras.  src_root lets
   engine binaries come from the engine bin and the project DLL from the project bin. */
static bool
ship_stage_binary( const dev_ship_desc_t* desc, const char* src_root, const char* out,
                   const char* name, const char* dst_name, bool required )
{
    char src[ SHIP_PATH_MAX ], dst[ SHIP_PATH_MAX ];
    snprintf( src, sizeof( src ), "%s" PATH_SEP "bin" PATH_SEP "%s", src_root, name );
    snprintf( dst, sizeof( dst ), "%s" PATH_SEP "bin" PATH_SEP "%s", out,
              dst_name ? dst_name : name );

    if ( !sys_file_exists( src ) )
    {
        if ( required )
        {
            ship_set_error( "'%s' not built (run the build stage first)", src );
            return false;
        }
        return true;
    }
    if ( !ship_copy_file( src, dst ) )
        return false;
    printf( "[stage]   bin" PATH_SEP "%s\n", dst_name ? dst_name : name );

    if ( desc->flags & DEV_SHIP_PDB )
    {
        char pdb_src[ SHIP_PATH_MAX ], pdb_dst[ SHIP_PATH_MAX ];
        snprintf( pdb_src, sizeof( pdb_src ), "%s", src );
        snprintf( pdb_dst, sizeof( pdb_dst ), "%s" PATH_SEP "bin" PATH_SEP "%s", out, name );
        char* dot = strrchr( pdb_src, '.' );
        if ( dot ) snprintf( dot, sizeof( pdb_src ) - ( dot - pdb_src ), ".pdb" );
        dot = strrchr( pdb_dst, '.' );
        if ( dot ) snprintf( dot, sizeof( pdb_dst ) - ( dot - pdb_dst ), ".pdb" );
        if ( sys_file_exists( pdb_src ) && !ship_copy_file( pdb_src, pdb_dst ) )
            return false;
    }
    return true;
}

/* Copy the content set into <out>/content/<name>.<ext>: a cooked file lands under its
   name in place of the source it came from, so the one content/ mount finds it. */
static bool
ship_stage_content( const dev_ship_desc_t* desc, const ship_images_t* img, const char* out )
{
    ship_set_t set;
    bool       ok = ship_set_build( desc, img, &set );

    for ( int i = 0; ok && i < set.count; ++i )
    {
        const ship_res_t* r = &set.items[ i ];
        char dst[ SHIP_PATH_MAX ];
        snprintf( dst, sizeof( dst ), "%s" PATH_SEP "content" PATH_SEP "%s.%s", out, r->name, r->ext );
        for ( char* c = dst + strlen( out ); *c; ++c )
            if ( *c == '/' ) *c = PATH_SEP[ 0 ];
        ok = ship_copy_file( r->src, dst );
    }
    if ( ok )
        printf( "[stage]   content (%d file%s: %d cooked, %d loose; %d manifest%s)\n",
                set.count, set.count == 1 ? "" : "s", set.cooked_count, set.count - set.cooked_count,
                set.manifests, set.manifests == 1 ? "" : "s" );

    ship_set_free( &set );
    return ok;
}

static bool
ship_stage( const dev_ship_desc_t* desc )
{
    char out[ SHIP_PATH_MAX ];
    ship_out_dir( desc, out, sizeof( out ) );
    printf( "[stage]   staging into %s\n", out );

    if ( desc->flags & DEV_SHIP_CLEAN )
    {
        int deleted = 0;
        sys_dir_walk( out, ship_delete_tree_files, &deleted );
        if ( deleted )
            printf( "[stage]   cleaned %d stale files\n", deleted );
    }

    if ( !sys_dir_make( out ) )
    {
        ship_set_error( "could not create staging dir '%s'", out );
        return false;
    }

    /* Engine root supplies the host exe, engine module DLLs and content; the project tree
       supplies the project's own DLL and content.  For an engine-resident target the two
       coincide. */
    const char* eng = ship_engine_root( desc );

    /* Light "without engine" shape: only the project's own game DLL rides along -- no host
       exe, engine module DLLs, content, or launcher.  The recipient runs it under their own
       engine (host_game -project <dir>), so nothing engine-side is bundled. */
    if ( desc->flags & DEV_SHIP_NO_ENGINE )
    {
        char dll[ SHIP_PATH_MAX ];
        snprintf( dll, sizeof( dll ), "%s.dll", desc->project );
        if ( !ship_stage_binary( desc, desc->root_dir, out, dll, NULL, true ) )
            return false;

        /* A run.bat so the project-only drop is still launchable: it runs under the engine's
           host, referenced by its ship-time absolute path (project-only is same-setup dev
           sharing -- edit the path for another install).  -project points the host at this
           staged dir; -module pins the DLL name regardless of the dir's name.  Only emitted
           for a child project, where the engine root is a real absolute path. */
        if ( eng != desc->root_dir )
        {
            char bat_path[ SHIP_PATH_MAX ], bat[ 1024 ];
            snprintf( bat_path, sizeof( bat_path ), "%s" PATH_SEP "%s.bat", out, desc->project );
            int n = snprintf( bat, sizeof( bat ),
                              "@echo off\r\n"
                              "rem project-only drop: runs under the engine it was shipped from.\r\n"
                              "cd /d \"%%~dp0\"\r\n"
                              "\"%s" PATH_SEP "bin" PATH_SEP "host_game.exe\""
                              " -project \"%%~dp0.\" -module %s %%*\r\n",
                              eng, desc->project );
            if ( !sys_file_write_entire( bat_path, bat, ( u32 )n ) )
            {
                ship_set_error( "could not write launcher '%s'", bat_path );
                return false;
            }
            printf( "[stage]   %s.bat\n", desc->project );
        }

        printf( "[stage]   project-only: recipient supplies the engine\n" );
        return true;
    }

    ship_images_t img;
    if ( !ship_images( desc, &img ) )
        return false;

    /* Executables + module DLLs. */
    bool modular = ( desc->flags & DEV_SHIP_MODULAR ) != 0;
    bool target  = ( desc->flags & DEV_SHIP_TARGET ) != 0;
    char name[ SHIP_PATH_MAX ];
    if ( modular )
    {
        snprintf( name, sizeof( name ), "%s.exe", img.exe );
        if ( !ship_stage_binary( desc, target ? desc->root_dir : eng, out, name, NULL, true ) )
            return false;
        for ( int i = 0; i < img.mod_count; ++i )
        {
            snprintf( name, sizeof( name ), "%s.dll", img.mods[ i ] );
            if ( !ship_stage_binary( desc, ship_module_root( desc, img.mods[ i ] ), out, name, NULL, true ) )
                return false;
        }
    }
    else
    {
        /* One exe.  A project's <project>_ship.exe ships as <project>.exe; a -target exe
           keeps its name. */
        char dst_name[ SHIP_PATH_MAX ];
        snprintf( name, sizeof( name ), "%s.exe", img.exe );
        snprintf( dst_name, sizeof( dst_name ), "%s.exe", desc->project );
        if ( !ship_stage_binary( desc, desc->root_dir, out, name, target ? NULL : dst_name, true ) )
            return false;
    }

    /* Content: exactly what the image's manifests name, closed over references.  Nothing
       else rides along -- gui hard-fails on a missing cooked shader or bake, so the set is
       checked complete here rather than hoped for. */
    if ( !ship_stage_content( desc, &img, out ) )
        return false;

    /* Root launcher: double-clickable, and for modular it carries the -module argument the
       host needs to find the project DLL. */
    {
        char bat_path[ SHIP_PATH_MAX ], bat[ 512 ];
        snprintf( bat_path, sizeof( bat_path ), "%s" PATH_SEP "%s.bat", out, desc->project );
        int n;
        if ( modular && !target )
            n = snprintf( bat, sizeof( bat ),
                          "@echo off\r\ncd /d \"%%~dp0\"\r\n"
                          "bin\\%s.exe -module %s %%*\r\n", img.exe, desc->project );
        else
            n = snprintf( bat, sizeof( bat ),
                          "@echo off\r\ncd /d \"%%~dp0\"\r\n"
                          "bin\\%s.exe %%*\r\n", desc->project );
        if ( !sys_file_write_entire( bat_path, bat, (u32)n ) )
        {
            ship_set_error( "could not write launcher '%s'", bat_path );
            return false;
        }
        printf( "[stage]   %s.bat\n", desc->project );
    }

    return true;
}

/*==============================================================================================
    package -- write the manifest.

    Walks the staged tree and writes <out>/manifest.txt: a build stamp header plus one
    `<crc32 hex> <size> <relative path>` line per file.  The foundation for integrity
    checks, patching, and "what build is this?" -- and cheap enough to write from day one.
    Zip bundling is additive later: staged loose files and pack-mounted bundles coexist
    behind the fs layer.
==============================================================================================*/

typedef struct
{
    const char* root;     /* staged out dir; rel paths are full_path past this */
    FILE*       fp;       /* open manifest being written */
    int         files;
    bool        ok;

} ship_manifest_ctx_t;

static bool
ship_manifest_cb( const char* filename, const char* full_path, void* userdata )
{
    ship_manifest_ctx_t* ctx = userdata;
    (void)filename;

    /* Skip the manifest itself: it is being written into the tree being walked. */
    const char* rel = full_path + strlen( ctx->root ) + 1;    /* +1 skips the joining sep */
    if ( strcmp( rel, "manifest.txt" ) == 0 )
        return true;

    sys_file_data_t fd = sys_file_read_entire( full_path );
    if ( !fd.ok )
    {
        ship_set_error( "manifest: could not read '%s'", full_path );
        ctx->ok = false;
        return false;
    }
    u32 crc = pack_crc32( 0, fd.data, fd.size );
    sys_file_free( &fd );

    /* Manifest paths are forward-slash regardless of platform. */
    char norm[ SHIP_PATH_MAX ];
    snprintf( norm, sizeof( norm ), "%s", rel );
    for ( char* p = norm; *p; ++p )
        if ( *p == '\\' )
            *p = '/';

    fprintf( ctx->fp, "%08x %10u %s\n", crc, sys_file_size( full_path ), norm );
    ++ctx->files;
    return true;
}

static bool
ship_package( const dev_ship_desc_t* desc )
{
    char out[ SHIP_PATH_MAX ], path[ SHIP_PATH_MAX ];
    ship_out_dir( desc, out, sizeof( out ) );
    snprintf( path, sizeof( path ), "%s" PATH_SEP "manifest.txt", out );

    FILE* fp = fopen( path, "wb" );
    if ( !fp )
    {
        ship_set_error( "could not write manifest '%s' (staged yet?)", path );
        return false;
    }

    SysDateTime dt;
    sys_datetime_local( &dt );
    fprintf( fp, "# %s %s (%s) -- built %04u-%02u-%02u %02u:%02u:%02u\n",
             desc->project, desc->config, ship_shape_name( desc ),
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second );
    fprintf( fp, "# crc32    size       path\n" );

    ship_manifest_ctx_t ctx = { .root = out, .fp = fp, .files = 0, .ok = true };
    sys_dir_walk( out, ship_manifest_cb, &ctx );
    fclose( fp );

    if ( !ctx.ok )
        return false;
    if ( ctx.files == 0 )
    {
        ship_set_error( "nothing staged in '%s' (run the stage stage first)", out );
        return false;
    }

    printf( "[package] manifest.txt (%d files)\n", ctx.files );
    return true;
}

/*==============================================================================================
    deploy -- publish the packaged layout to its destination.

    A directory publish that RECONCILES rather than clears.  Blanket-wiping a user-supplied
    destination is the one line a packaging tool must never have: a mistyped path destroys
    data, it deletes legitimate neighbors of a shipped game (saves, logs), and a recursive
    mass-delete is the access pattern EDR heuristics flag.  Instead the manifest is the
    record of what we shipped last time:

        1. checks first, no mutation: the staged manifest must exist (package ran), and the
           destination must be empty, absent, or carry a manifest of ours -- a non-empty
           dir without one is refused, not overwritten
        2. delete exactly the files the OLD manifest lists that the NEW one does not --
           exact paths, no wildcards, never anything we did not previously ship
        3. copy the staged tree over

    The old destination manifest is deleted between 2 and 3, so an interrupted deploy
    tends to leave a visibly manifest-less (= invalid) build rather than a plausible one.
==============================================================================================*/

/* Iterate manifest payload lines in place.  `*cursor` walks a writable NUL-terminated
   buffer (sys_file_read_entire guarantees the terminator); each call terminates the
   current line and returns its path column, skipping '#' comments.  Lines are fixed
   format -- "%08x %10u %s" -- so the path starts at column 20 (paths may contain spaces,
   which is why the format is fixed-width, not token-split). */
static const char*
ship_manifest_next_path( char** cursor )
{
    while ( **cursor )
    {
        char* line = *cursor;
        char* nl   = strchr( line, '\n' );
        if ( nl )
            *nl = '\0';
        *cursor = nl ? nl + 1 : line + strlen( line );

        if ( line[ 0 ] != '#' && strlen( line ) > 20 )
            return line + 20;
    }
    return NULL;
}

/* True if `path` appears in the manifest text (probed fresh per call; counts are tiny). */
static bool
ship_manifest_contains( const char* manifest_text, u32 text_size, const char* path )
{
    /* Search for the fixed-format line tail directly, then confirm the hit sits in the
       path column of its line. */
    char needle[ SHIP_PATH_MAX ];
    snprintf( needle, sizeof( needle ), " %s\n", path );
    const char* hit = manifest_text;
    size_t      nlen = strlen( needle );
    (void)text_size;
    while ( ( hit = strstr( hit, needle ) ) != NULL )
    {
        /* Column check: the path column starts at offset 20 into its line. */
        const char* line = hit + 1;
        while ( line > manifest_text && line[ -1 ] != '\n' )
            --line;
        if ( ( hit + 1 ) - line == 20 )
            return true;
        hit += nlen;
    }
    return false;
}

static bool
ship_count_files_cb( const char* filename, const char* full_path, void* userdata )
{
    (void)filename; (void)full_path;
    ++*(int*)userdata;
    return true;
}

static bool
ship_deploy( const dev_ship_desc_t* desc )
{
    if ( !desc->deploy_dir || !desc->deploy_dir[ 0 ] )
    {
        printf( "[deploy]  no destination (-deploy <dir>); the staged dir is the deliverable\n" );
        return true;
    }

    char out[ SHIP_PATH_MAX ], new_path[ SHIP_PATH_MAX ], old_path[ SHIP_PATH_MAX ];
    ship_out_dir( desc, out, sizeof( out ) );
    snprintf( new_path, sizeof( new_path ), "%s" PATH_SEP "manifest.txt", out );
    snprintf( old_path, sizeof( old_path ), "%s" PATH_SEP "manifest.txt", desc->deploy_dir );

    /* --- checks first: nothing at the destination is touched until all pass --- */

    sys_file_data_t new_man = sys_file_read_entire( new_path );
    if ( !new_man.ok )
    {
        ship_set_error( "no staged manifest '%s' (run the package stage first)", new_path );
        return false;
    }

    sys_file_data_t old_man = sys_file_read_entire( old_path );
    if ( !old_man.ok )
    {
        /* No manifest: only an empty or absent destination is safe to adopt.  Anything
           else is not a build we deployed -- refuse rather than overwrite. */
        int existing = 0;
        sys_dir_walk( desc->deploy_dir, ship_count_files_cb, &existing );
        if ( existing > 0 )
        {
            ship_set_error( "'%s' has %d files but no manifest.txt -- not a deployed build;"
                            " refusing to modify it (choose an empty or new directory)",
                            desc->deploy_dir, existing );
            sys_file_free( &new_man );
            return false;
        }
    }

    /* --- reconcile: remove exactly what the old manifest shipped and the new one drops --- */

    int removed = 0;
    if ( old_man.ok )
    {
        char*       cursor = (char*)old_man.data;
        const char* rel;
        while ( ( rel = ship_manifest_next_path( &cursor ) ) != NULL )
        {
            if ( ship_manifest_contains( (const char*)new_man.data, new_man.size, rel ) )
                continue;

            char victim[ SHIP_PATH_MAX ];
            snprintf( victim, sizeof( victim ), "%s" PATH_SEP "%s", desc->deploy_dir, rel );
            if ( sys_file_delete( victim ) )
            {
                printf( "[deploy]  removed stale %s\n", rel );
                ++removed;
            }
        }

        /* Old manifest last: while the copy below runs, the destination has no manifest,
           so an interrupted deploy reads as invalid instead of plausible. */
        sys_file_delete( old_path );
    }
    sys_file_free( &old_man );
    sys_file_free( &new_man );

    /* --- publish --- */

    int copied = 0;
    if ( !ship_copy_tree( out, desc->deploy_dir, &copied ) )
        return false;

    printf( "[deploy]  %s (%d files, %d stale removed)\n", desc->deploy_dir, copied, removed );
    return true;
}

/*==============================================================================================
    Dispatch
==============================================================================================*/

bool
dev_ship_run_stage( const dev_ship_desc_t* desc, dev_ship_stage_t stage )
{
    if ( !ship_validate( desc ) )
        return false;

    switch ( stage )
    {
        case DEV_SHIP_BUILD:   return ship_build( desc );
        case DEV_SHIP_VERIFY:  return ship_verify( desc );
        case DEV_SHIP_STAGE:   return ship_stage( desc );
        case DEV_SHIP_PACKAGE: return ship_package( desc );
        case DEV_SHIP_DEPLOY:  return ship_deploy( desc );
        default:
            ship_set_error( "unknown stage %d", (int)stage );
            return false;
    }
}

bool
dev_ship_run( const dev_ship_desc_t* desc )
{
    for ( int i = 0; i < DEV_SHIP_STAGE_COUNT; ++i )
        if ( !dev_ship_run_stage( desc, (dev_ship_stage_t)i ) )
            return false;
    return true;
}
