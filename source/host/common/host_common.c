/*==============================================================================================

    host/common/host_common.c : Pre-runtime host setup implementation.

==============================================================================================*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "orb.h"
#include "host/common/host_common.h"

/*==============================================================================================
    API
==============================================================================================*/

void
host_args_parse( int argc, char** argv, launch_params_t* out )
{
    memset( out, 0, sizeof( *out ) );

    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-module" ) == 0 && i + 1 < argc )
        {
            snprintf( out->module_override, sizeof( out->module_override ), "%s", argv[ ++i ] );
        }
        else if ( strcmp( argv[ i ], "-project" ) == 0 && i + 1 < argc )
        {
            snprintf( out->project_path, sizeof( out->project_path ), "%s", argv[ ++i ] );
        }
        else if ( strcmp( argv[ i ], "-dev" ) == 0 )
        {
            out->dev_mode = true;
        }
    }
}

/*==============================================================================================
    Project Resolution
==============================================================================================*/

/* Mirrors the module system's MODULE_NAME_MAX (mod_internal.c) minus the terminator.
   Duplicated by value, not by include -- this file stays stdlib-only, pre-engine. */
#define HOST_PROJECT_NAME_MAX 31

#ifdef _WIN32
    #define HOST_PATH_SEP '\\'
#else
    #define HOST_PATH_SEP '/'
#endif

/* Engine module names a project must not collide with: mod_dynamic_load treats an
   existing slot name as "already loaded" and returns success, so a project named
   'render' would silently alias the render module and the host would then cast the
   wrong api struct.  Reject up front with a clear message instead. */
static const char* k_reserved_modules[] = {
    "mod", "sys", "ref", "res", "prof", "pack", "fs", "core", "job", "net", "app", "run",
    "rhi", "draw", "gui", "asset", "input", "ahi",
    "render", "audio", "physics", "game", "example",
};

static bool
project_dir_exists( const char* path )
{
    struct stat st;
    if ( stat( path, &st ) != 0 )
        return false;
    return ( st.st_mode & S_IFMT ) == S_IFDIR;
}

static bool
project_file_exists( const char* path )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;
    fclose( f );
    return true;
}

/* Read the project's authoritative module name from its orb.targets: the first
   'target <name>' block (blocks begin at column 0).  That name is what -create passed
   and what names the built dll, and it need NOT match the directory -- e.g.
   '-create widget_game -dir games/dircheck' builds widget_game.dll under games/dircheck.
   The directory basename is only a fallback for a project without a readable orb.targets.
   Returns true and fills out on a hit.  Stays stdlib-only like the rest of this file. */
static bool
project_name_from_targets( const char* proj_root, char* out, size_t out_size )
{
    char tpath[ HOST_PATH_MAX + 16 ];
    snprintf( tpath, sizeof( tpath ), "%s%corb.targets", proj_root, HOST_PATH_SEP );

    FILE* f = fopen( tpath, "r" );
    if ( !f )
        return false;

    bool found = false;
    char line[ 512 ];
    while ( fgets( line, sizeof( line ), f ) )
    {
        /* Keyword must sit at column 0 (a 'run host_game -project .' value never does). */
        if ( strncmp( line, "target", 6 ) != 0 || ( line[ 6 ] != ' ' && line[ 6 ] != '\t' ) )
            continue;

        const char* q = line + 6;
        while ( *q == ' ' || *q == '\t' ) ++q;

        size_t n = 0;
        while ( q[ n ] && q[ n ] != ' ' && q[ n ] != '\t' && q[ n ] != '\r' &&
                q[ n ] != '\n' && q[ n ] != '#' && n + 1 < out_size )
            ++n;

        if ( n > 0 )
        {
            snprintf( out, out_size, "%.*s", ( int )n, q );
            found = true;
        }
        break;    /* the first target block is the game dll target */
    }
    fclose( f );
    return found;
}

bool
host_resolve_project( const launch_params_t* params, host_project_t* out,
                      char* err, size_t err_size )
{
    memset( out, 0, sizeof( *out ) );
    if ( err && err_size )
        err[ 0 ] = '\0';

    const bool has_module  = params->module_override[ 0 ] != '\0';
    const bool has_project = params->project_path[ 0 ]    != '\0';

    if ( !has_module && !has_project )
        return true; /* nothing requested -- out->present stays false */

    /* ---- name ------------------------------------------------------- */

    if ( has_project )
    {
        /* Absolutize so the mod system's dir survives any later cwd change. */
        char abs[ HOST_PATH_MAX ];
#ifdef _WIN32
        if ( !_fullpath( abs, params->project_path, sizeof( abs ) ) )
        {
            snprintf( err, err_size, "-project path could not be resolved: %s", params->project_path );
            return false;
        }
#else
        /* realpath(path, NULL) mallocs a PATH_MAX buffer -- safe regardless of
           HOST_PATH_MAX; copy out and release. */
        char* resolved = realpath( params->project_path, NULL );
        if ( !resolved )
        {
            snprintf( err, err_size, "-project path could not be resolved: %s", params->project_path );
            return false;
        }
        snprintf( abs, sizeof( abs ), "%s", resolved );
        free( resolved );
#endif

        /* Strip trailing separators, then take the basename as the default name. */
        size_t len = strlen( abs );
        while ( len > 1 && ( abs[ len - 1 ] == '\\' || abs[ len - 1 ] == '/' ) )
            abs[ --len ] = '\0';

        const char* base = abs;
        for ( const char* p = abs; *p; ++p )
            if ( *p == '\\' || *p == '/' )
                base = p + 1;

        if ( !base[ 0 ] )
        {
            snprintf( err, err_size, "-project path has no directory name: %s", params->project_path );
            return false;
        }

        /* Default the module name from the project's orb.targets (authoritative: it is
           what -create named the target and thus the dll), and fall back to the directory
           basename only when orb.targets is unreadable or declares no target.  An explicit
           -module always wins, so a mismatched or missing targets file can still be run. */
        char targets_name[ HOST_MODULE_MAX ];
        if ( has_module )
            snprintf( out->name, sizeof( out->name ), "%s", params->module_override );
        else if ( project_name_from_targets( abs, targets_name, sizeof( targets_name ) ) )
            snprintf( out->name, sizeof( out->name ), "%s", targets_name );
        else
            snprintf( out->name, sizeof( out->name ), "%s", base );

        /* Project layout: dll in <path>/bin (the child-project build output), else
           flat beside the project files. */
        char bin_dir[ HOST_PATH_MAX ];
        snprintf( bin_dir, sizeof( bin_dir ), "%s%cbin", abs, HOST_PATH_SEP );
        if ( project_dir_exists( bin_dir ) )
            snprintf( out->dir, sizeof( out->dir ), "%s", bin_dir );
        else
            snprintf( out->dir, sizeof( out->dir ), "%s", abs );
    }
    else
    {
        /* -module only: dll expected beside the host exe; the mod system's default
           root already points there, so dir stays "". */
        snprintf( out->name, sizeof( out->name ), "%s", params->module_override );
    }

    /* ---- validation --------------------------------------------------- */

    if ( strlen( out->name ) > HOST_PROJECT_NAME_MAX )
    {
        snprintf( err, err_size, "project name too long (max %d): %s",
                  HOST_PROJECT_NAME_MAX, out->name );
        return false;
    }

    for ( size_t i = 0; i < sizeof( k_reserved_modules ) / sizeof( k_reserved_modules[ 0 ] ); ++i )
    {
        if ( strcmp( out->name, k_reserved_modules[ i ] ) == 0 )
        {
            snprintf( err, err_size,
                      "'%s' is a reserved engine module name; rename the project or pass "
                      "-module <name> to pick a different dll",
                      out->name );
            return false;
        }
    }

    /* Pre-check the dll so a typo'd path fails with the exact file named, not a
       generic loader error late in boot.  -module-only skips this (dll sits in the
       exe dir; the mod loader's error is already exact there). */
    if ( out->dir[ 0 ] )
    {
        char dll[ HOST_PATH_MAX + HOST_MODULE_MAX + 8 ];
        snprintf( dll, sizeof( dll ), "%s%c%s.dll", out->dir, HOST_PATH_SEP, out->name );
        if ( !project_file_exists( dll ) )
        {
            snprintf( err, err_size,
                      "project dll not found: %s (build the project, or pass -module <name> "
                      "if the dll name differs from the orb.targets target)",
                      dll );
            return false;
        }
    }

    out->present = true;
    return true;
}

/*============================================================================================*/