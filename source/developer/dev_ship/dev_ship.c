/*==============================================================================================

    dev_ship.c -- Developer ship-pipeline library.

    See dev_ship.h for the pipeline contract.  Each stage is a static function behind the
    dev_ship_run_stage() dispatcher; dev_ship_run() drives all stages in order and stops at
    the first failure.  Errors land in a static message buffer (single-threaded tool code,
    same convention as dev_image).

    Current state: build shells out to build_tool for the <project>_ship monolithic exe;
    cook is a real no-op (all cooking is offline and committed); stage, package, and deploy
    are scaffolds that log and succeed so the pipeline runs end to end while they land.

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
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
    build -- compile the ship-shape exe via build_tool.

    The ship target is <project>_ship (see the PATTERN block in orb.targets): one generic
    host source with the project baked in, built -monolithic so the project archives as a
    static lib and the result is a single exe with hot-reload a no-op.
==============================================================================================*/

static bool
ship_build( const dev_ship_desc_t* desc )
{
    if ( desc->flags & DEV_SHIP_SKIP_BUILD )
    {
        printf( "[build]   skipped (-skip-build): staging prebuilt bin output\n" );
        return true;
    }

    char cmd[ 1024 ];
    snprintf( cmd, sizeof( cmd ),
              "\"%s" PATH_SEP "bin" PATH_SEP BUILD_TOOL_EXE "\""
              " -monolithic -config %s -target %s_ship",
              desc->root_dir, desc->config, desc->project );

    printf( "[build]   %s\n", cmd );

    sys_process_result_t result;
    if ( !sys_process_run( cmd, desc->root_dir, &result ) )
    {
        ship_set_error( "could not launch build_tool: %s", cmd );
        return false;
    }
    if ( result.exit_code != 0 )
    {
        ship_set_error( "build_tool failed (exit %d)", result.exit_code );
        return false;
    }

    printf( "[build]   %s_ship %s built (%.1fs)\n",
            desc->project, desc->config, result.elapsed_seconds );
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

    TODO: copy the ship exe, assets/font + assets/icon (cooked runtime assets only),
    config/, and cooked shaders into the ship layout (exe at root, data beside it).
==============================================================================================*/

static bool
ship_stage( const dev_ship_desc_t* desc )
{
    char out[ 512 ];
    ship_out_dir( desc, out, sizeof( out ) );

    printf( "[stage]   TODO: stage %s_ship into %s\n", desc->project, out );
    return true;
}

/*==============================================================================================
    package -- write the manifest and (later) bundle assets into zip packs.

    TODO: walk the staged tree and write manifest.txt (file list + size + crc32 + build
    stamp).  Zip bundling is additive later -- staged loose files and pack-mounted bundles
    coexist behind the fs layer.
==============================================================================================*/

static bool
ship_package( const dev_ship_desc_t* desc )
{
    (void)desc;
    printf( "[package] TODO: write manifest\n" );
    return true;
}

/*==============================================================================================
    deploy -- publish the packaged layout to its destination.

    TODO: v0 target is a directory publish; the staged dir already is the deliverable, so
    this stays a no-op until a destination other than out_dir exists.
==============================================================================================*/

static bool
ship_deploy( const dev_ship_desc_t* desc )
{
    (void)desc;
    printf( "[deploy]  TODO: directory publish\n" );
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
