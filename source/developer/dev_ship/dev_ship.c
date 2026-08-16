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
#include <stdlib.h>   /* atoi -- fonts.manifest size field */
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
    #define FONT_TOOL_EXE   "font_tool.exe"
#else
    #define PATH_SEP        "/"
    #define BUILD_TOOL_EXE  "build_tool"
    #define FONT_TOOL_EXE   "font_tool"
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

/* Human label for the ship shape -- used in build logs and the manifest header. */
static const char*
ship_shape_name( const dev_ship_desc_t* desc )
{
    if ( desc->flags & DEV_SHIP_NO_ENGINE ) return "project-only";
    if ( desc->flags & DEV_SHIP_MODULAR )   return "modular";
    return "monolithic";
}

/* Engine root: supplies build_tool, and (with-engine) the host exe, module DLLs, and assets.
   Returns desc->root_dir when engine_dir is unset -- an engine-resident target, where the two
   roots coincide.  A child project sets engine_dir (from .orb_engine), so the returned pointer
   differs from root_dir: that inequality is exactly the "is this a child project?" test. */
static const char*
ship_engine_root( const dev_ship_desc_t* desc )
{
    return ( desc->engine_dir && desc->engine_dir[ 0 ] ) ? desc->engine_dir : desc->root_dir;
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
    if ( ship_engine_root( desc ) != desc->root_dir )
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

    printf( "[build]   %s %s (%s) built\n", desc->project, desc->config, ship_shape_name( desc ) );
    return true;
}

/*==============================================================================================
    cook -- convert source assets to runtime formats.

    Fonts cook from config/fonts.manifest: one line per shipped bake,

        <family> <size_px> [-sdf[=n]] [-range=<spec>]

    where <family> is a font_tool input (a TTF in assets/font_source, or an OS-installed face
    name -- wrap names containing spaces in double quotes) and the flags pass through to
    font_tool verbatim.  '#' starts a comment.  Each line runs font_tool.exe (FreeType
    quality), whose default output dir is the engine's assets/font/ -- which the stage step
    then ships wholesale, so a cooked font needs no further wiring.  font_tool's own cache-less
    bake runs every time; the tool is fast and a ship is not an inner loop.

    No manifest = nothing to cook.  Shaders and other assets are still cooked offline by their
    tools; their call-outs land here the same way when the time comes.
==============================================================================================*/

static bool
ship_cook( const dev_ship_desc_t* desc )
{
    char manifest[ 1024 ];
    snprintf( manifest, sizeof( manifest ),
              "%s" PATH_SEP "config" PATH_SEP "fonts.manifest", desc->root_dir );

    FILE* f = fopen( manifest, "rb" );
    if ( !f )
    {
        printf( "[cook]    nothing to cook (no config/fonts.manifest)\n" );
        return true;
    }

    /* font_tool.exe lives in the engine bin; build it if this tree never has. */
    const char* eng = ship_engine_root( desc );
    char        tool[ 1024 ];
    snprintf( tool, sizeof( tool ), "%s" PATH_SEP "bin" PATH_SEP FONT_TOOL_EXE, eng );
    if ( sys_file_time( tool ) == 0 && !ship_build_target( desc, "font_tool", false ) )
    {
        fclose( f );
        return false;
    }

    char line[ 512 ];
    int  baked = 0;
    bool ok    = true;

    while ( ok && fgets( line, sizeof( line ), f ) )
    {
        char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;
        if ( *p == '#' || *p == '\0' || *p == '\r' || *p == '\n' )
            continue;

        size_t len = strlen( p );
        while ( len && ( p[ len - 1 ] == '\n' || p[ len - 1 ] == '\r'
                      || p[ len - 1 ] == ' '  || p[ len - 1 ] == '\t' ) )
            p[ --len ] = '\0';

        /* Family: a leading '"' opens a quoted span (names contain spaces), else the first
           whitespace-delimited token. */
        char        family[ 256 ];
        const char* rest;
        if ( *p == '"' )
        {
            char* q = strchr( p + 1, '"' );
            if ( !q )
            {
                ship_set_error( "fonts.manifest: unterminated quote: %s", p );
                ok = false;
                break;
            }
            snprintf( family, sizeof( family ), "%.*s", (int)( q - p - 1 ), p + 1 );
            rest = q + 1;
        }
        else
        {
            const char* q = p;
            while ( *q && *q != ' ' && *q != '\t' ) ++q;
            snprintf( family, sizeof( family ), "%.*s", (int)( q - p ), p );
            rest = q;
        }
        while ( *rest == ' ' || *rest == '\t' ) ++rest;

        int size_px = atoi( rest );
        if ( size_px < 6 || size_px > 256 )
        {
            ship_set_error( "fonts.manifest: bad size in line: %s", p );
            ok = false;
            break;
        }

        /* Flags: everything after the size token, passed through verbatim. */
        const char* flags = rest;
        while ( *flags && *flags != ' ' && *flags != '\t' ) ++flags;
        while ( *flags == ' ' || *flags == '\t' ) ++flags;

        char cmd[ 1600 ];
        snprintf( cmd, sizeof( cmd ), "\"%s\" \"%s\" %d%s%s",
                  tool, family, size_px, *flags ? " " : "", flags );

        sys_process_result_t res;
        if ( !sys_process_run( cmd, desc->root_dir, &res ) )
        {
            ship_set_error( "could not launch font_tool: %s", cmd );
            ok = false;
            break;
        }
        if ( res.exit_code != 0 )
        {
            ship_set_error( "font_tool failed (exit %d) on manifest line: %s",
                            res.exit_code, p );
            ok = false;
            break;
        }
        printf( "[cook]    font %s %dpx%s%s -> assets/font/\n",
                family, size_px, *flags ? " " : "", flags );
        ++baked;
    }
    fclose( f );

    if ( ok )
        printf( "[cook]    %d font bake%s from config/fonts.manifest\n",
                baked, baked == 1 ? "" : "s" );
    return ok;
}

/*==============================================================================================
    stage -- gather the runtime file set into the staging directory.

    The staged layout mirrors the dev tree (see dev_ship.h): exes in <out>/bin because
    sys_root_dir() resolves the asset root one level above the executable, so assets/ and
    config/ beside bin/ are found with zero path changes.  Monolithic ships one renamed exe;
    modular ships host_game.exe + the module DLLs with their build names (kept so the DLL
    loader and .pdb references resolve) plus a launcher .bat carrying the -module argument.
==============================================================================================*/

/* Copy <src_root>/bin/<name> to <out>/bin/<dst_name> (NULL = keep the name); with DEV_SHIP_PDB
   also copy the like-named .pdb, under its BUILD name so the exe's embedded reference still
   resolves.  `required` distinguishes "not built" (error) from optional extras.  src_root lets
   engine binaries come from the engine bin and the project DLL from the project bin. */
static bool
ship_stage_binary( const dev_ship_desc_t* desc, const char* src_root, const char* out,
                   const char* name, const char* dst_name, bool required )
{
    char src[ 512 ], dst[ 512 ];
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

/* Copy a data subtree; 0 files is a warning, not an error (meager-content projects).
   src_root is the tree the data comes from (the engine tree for a child project's assets). */
static bool
ship_stage_tree( const dev_ship_desc_t* desc, const char* src_root, const char* out, const char* rel )
{
    (void)desc;
    char src[ 512 ], dst[ 512 ];
    snprintf( src, sizeof( src ), "%s" PATH_SEP "%s", src_root, rel );
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

    /* Light "without engine" shape: only the project's own game DLL rides along -- no host
       exe, engine module DLLs, assets, or launcher.  The recipient runs it under their own
       engine (host_game -project <dir>), so nothing engine-side is bundled. */
    /* Engine root supplies the host exe, engine module DLLs, and assets; the project bin
       supplies the project's own DLL.  For an engine-resident target the two coincide. */
    const char* eng = ship_engine_root( desc );

    if ( desc->flags & DEV_SHIP_NO_ENGINE )
    {
        char dll[ 512 ];
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
            char bat_path[ 512 ], bat[ 1024 ];
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

    /* Executables + module DLLs. */
    bool modular = ( desc->flags & DEV_SHIP_MODULAR ) != 0;
    char name[ 512 ];
    if ( modular )
    {
        if ( !ship_stage_binary( desc, eng, out, "host_game.exe", NULL, true ) )
            return false;
        for ( int i = 0; i < (int)ARRAY_COUNT( s_runtime_modules ); ++i )
        {
            snprintf( name, sizeof( name ), "%s.dll", s_runtime_modules[ i ] );
            if ( !ship_stage_binary( desc, eng, out, name, NULL, true ) )
                return false;
        }
        snprintf( name, sizeof( name ), "%s.dll", desc->project );
        if ( !ship_stage_binary( desc, desc->root_dir, out, name, NULL, true ) )
            return false;
    }
    else
    {
        char dst_name[ 512 ];
        snprintf( name, sizeof( name ), "%s_ship.exe", desc->project );
        snprintf( dst_name, sizeof( dst_name ), "%s.exe", desc->project );
        if ( !ship_stage_binary( desc, eng, out, name, dst_name, true ) )
            return false;
    }

    /* Cooked runtime data, from the engine tree.  Sources and caches (font_source, font_cache,
       icon_source, ...) deliberately stay home; cooked shaders ride along when present (missing
       is fine -- the embedded SPIR-V fallback covers them). */
    if ( !ship_stage_tree( desc, eng, out, "assets" PATH_SEP "font" ) ) return false;
    if ( !ship_stage_tree( desc, eng, out, "assets" PATH_SEP "icon" ) ) return false;
    if ( !ship_stage_tree( desc, eng, out, "config" ) )                 return false;
    {
        char src[ 512 ], dst[ 512 ];
        snprintf( src, sizeof( src ), "%s" PATH_SEP "bin" PATH_SEP "shaders", eng );
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
    /* Work on a stack copy of the line-iterated state: re-scan from the start each call
       by searching for the fixed-format line tail directly. */
    char needle[ 512 ];
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

    char out[ 512 ], new_path[ 512 ], old_path[ 512 ];
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

            char victim[ 512 ];
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
