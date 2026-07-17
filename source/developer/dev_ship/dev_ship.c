/*==============================================================================================

    dev_ship.c -- Developer ship-pipeline library.

    See dev_ship.h for the pipeline contract.  Each stage is a static function behind the
    dev_ship_run_stage() dispatcher; dev_ship_run() drives all stages in order and stops at
    the first failure.  Errors land in a static message buffer (single-threaded tool code,
    same convention as dev_image).

    Current state: build shells out to build_tool (monolithic <project>_ship by default,
    host_game + module DLLs with DEV_SHIP_MODULAR -- build_tool's per-(config,mode) stamps
    make mode switches rebuild correctly, no -force needed); cook is a real no-op (all
    cooking is offline and committed); stage copies the runtime file set into the shipped
    layout; package and deploy are scaffolds that log and succeed.

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/pack/pack_host.h"    /* pack_crc32 for the manifest */
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
    [ DEV_SHIP_COOK ]    = "cook",
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

/* Copy one file, creating the destination's parent directories first. */
static bool
ship_copy_file( const char* src, const char* dst )
{
    char parent[ 512 ];
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

    char dst[ 512 ];
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
   failed copy; a missing/empty source is 0 copies, not an error (callers decide whether
   that deserves a warning). */
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
    char probe[ 512 ];
    snprintf( probe, sizeof( probe ), "%s" PATH_SEP "orb.targets", desc->root_dir );
    if ( !sys_file_exists( probe ) )
    {
        ship_set_error( "'%s' is not the engine root (no orb.targets)", desc->root_dir );
        return false;
    }
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
==============================================================================================*/

/* Runtime module DLLs a modular host_game loads -- mirrors the ship target's mono_dep list
   in orb.targets (render game <project>).  TODO: parse that list instead of mirroring it. */
static const char* s_runtime_modules[] = { "render", "game" };

static bool
ship_build_target( const dev_ship_desc_t* desc, const char* target, bool monolithic )
{
    char cmd[ 1024 ];
    snprintf( cmd, sizeof( cmd ),
              "\"%s" PATH_SEP "bin" PATH_SEP BUILD_TOOL_EXE "\"%s -config %s -target %s",
              desc->root_dir, monolithic ? " -monolithic" : "", desc->config, target );

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

    if ( desc->flags & DEV_SHIP_MODULAR )
    {
        if ( !ship_build_target( desc, "host_game", false ) )
            return false;
        for ( int i = 0; i < (int)ARRAY_COUNT( s_runtime_modules ); ++i )
            if ( !ship_build_target( desc, s_runtime_modules[ i ], false ) )
                return false;
        if ( !ship_build_target( desc, desc->project, false ) )
            return false;
    }
    else
    {
        char target[ 256 ];
        snprintf( target, sizeof( target ), "%s_ship", desc->project );
        if ( !ship_build_target( desc, target, true ) )
            return false;
    }

    printf( "[build]   %s %s (%s) built\n", desc->project, desc->config,
            ( desc->flags & DEV_SHIP_MODULAR ) ? "modular" : "monolithic" );
    return true;
}

/*==============================================================================================
    cook -- convert source assets to runtime formats.

    A real no-op today: fonts, shaders, and assets are cooked offline by their tools and the
    results committed.  This stage becomes the font_tool/shader_tool/asset_tool call-out
    once cooking moves into the pipeline.
==============================================================================================*/

static bool
ship_cook( const dev_ship_desc_t* desc )
{
    (void)desc;
    printf( "[cook]    nothing to cook (assets are cooked offline)\n" );
    return true;
}

/*==============================================================================================
    stage -- gather the runtime file set into the staging directory.

    The staged layout mirrors the dev tree (see dev_ship.h): exes in <out>/bin because
    sys_root_dir() resolves the asset root one level above the executable, so assets/ and
    config/ beside bin/ are found with zero path changes.  Monolithic ships one renamed exe;
    modular ships host_game.exe + the module DLLs with their build names (kept so the DLL
    loader and .pdb references resolve) plus a launcher .bat carrying the -module argument.
==============================================================================================*/

/* Copy <root>/bin/<name> to <out>/bin/<dst_name> (NULL = keep the name); with DEV_SHIP_PDB
   also copy the like-named .pdb, under its BUILD name so the exe's embedded reference still
   resolves.  `required` distinguishes "not built" (error) from optional extras. */
static bool
ship_stage_binary( const dev_ship_desc_t* desc, const char* out, const char* name,
                   const char* dst_name, bool required )
{
    char src[ 512 ], dst[ 512 ];
    snprintf( src, sizeof( src ), "%s" PATH_SEP "bin" PATH_SEP "%s", desc->root_dir, name );
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
        char pdb_src[ 512 ], pdb_dst[ 512 ];
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

/* Copy a data subtree; 0 files is a warning, not an error (meager-content projects). */
static bool
ship_stage_tree( const dev_ship_desc_t* desc, const char* out, const char* rel )
{
    char src[ 512 ], dst[ 512 ];
    snprintf( src, sizeof( src ), "%s" PATH_SEP "%s", desc->root_dir, rel );
    snprintf( dst, sizeof( dst ), "%s" PATH_SEP "%s", out, rel );

    int copied = 0;
    if ( !ship_copy_tree( src, dst, &copied ) )
        return false;
    if ( copied )
        printf( "[stage]   %s (%d files)\n", rel, copied );
    else
        printf( "[stage]   warn: %s is missing or empty\n", rel );
    return true;
}

static bool
ship_stage( const dev_ship_desc_t* desc )
{
    char out[ 512 ];
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

    /* Executables + module DLLs. */
    bool modular = ( desc->flags & DEV_SHIP_MODULAR ) != 0;
    char name[ 512 ];
    if ( modular )
    {
        if ( !ship_stage_binary( desc, out, "host_game.exe", NULL, true ) )
            return false;
        for ( int i = 0; i < (int)ARRAY_COUNT( s_runtime_modules ); ++i )
        {
            snprintf( name, sizeof( name ), "%s.dll", s_runtime_modules[ i ] );
            if ( !ship_stage_binary( desc, out, name, NULL, true ) )
                return false;
        }
        snprintf( name, sizeof( name ), "%s.dll", desc->project );
        if ( !ship_stage_binary( desc, out, name, NULL, true ) )
            return false;
    }
    else
    {
        char dst_name[ 512 ];
        snprintf( name, sizeof( name ), "%s_ship.exe", desc->project );
        snprintf( dst_name, sizeof( dst_name ), "%s.exe", desc->project );
        if ( !ship_stage_binary( desc, out, name, dst_name, true ) )
            return false;
    }

    /* Cooked runtime data.  Sources and caches (font_source, font_cache, icon_source, ...)
       deliberately stay home; cooked shaders ride along when present (missing is fine --
       the embedded SPIR-V fallback covers them). */
    if ( !ship_stage_tree( desc, out, "assets" PATH_SEP "font" ) ) return false;
    if ( !ship_stage_tree( desc, out, "assets" PATH_SEP "icon" ) ) return false;
    if ( !ship_stage_tree( desc, out, "config" ) )                 return false;
    {
        char src[ 512 ], dst[ 512 ];
        snprintf( src, sizeof( src ), "%s" PATH_SEP "bin" PATH_SEP "shaders", desc->root_dir );
        snprintf( dst, sizeof( dst ), "%s" PATH_SEP "bin" PATH_SEP "shaders", out );
        int copied = 0;
        if ( !ship_copy_tree( src, dst, &copied ) )
            return false;
        if ( copied )
            printf( "[stage]   bin" PATH_SEP "shaders (%d files)\n", copied );
    }

    /* Root launcher: double-clickable, and for modular it carries the -module argument the
       host needs to find the project DLL. */
    {
        char bat_path[ 512 ], bat[ 512 ];
        snprintf( bat_path, sizeof( bat_path ), "%s" PATH_SEP "%s.bat", out, desc->project );
        int n;
        if ( modular )
            n = snprintf( bat, sizeof( bat ),
                          "@echo off\r\ncd /d \"%%~dp0\"\r\n"
                          "bin\\host_game.exe -module %s %%*\r\n", desc->project );
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
    char norm[ 512 ];
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
    char out[ 512 ], path[ 512 ];
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
             desc->project, desc->config,
             ( desc->flags & DEV_SHIP_MODULAR ) ? "modular" : "monolithic",
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

    v0 is a directory publish: mirror the staged tree into desc->deploy_dir.  With no
    destination set this is a stated no-op -- the staged dir already is the deliverable.
==============================================================================================*/

static bool
ship_deploy( const dev_ship_desc_t* desc )
{
    if ( !desc->deploy_dir || !desc->deploy_dir[ 0 ] )
    {
        printf( "[deploy]  no destination (-deploy <dir>); the staged dir is the deliverable\n" );
        return true;
    }

    char out[ 512 ];
    ship_out_dir( desc, out, sizeof( out ) );

    int copied = 0;
    if ( !ship_copy_tree( out, desc->deploy_dir, &copied ) )
        return false;
    if ( copied == 0 )
    {
        ship_set_error( "nothing staged in '%s' (run the stage stage first)", out );
        return false;
    }

    printf( "[deploy]  %s (%d files)\n", desc->deploy_dir, copied );
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
        case DEV_SHIP_COOK:    return ship_cook( desc );
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
