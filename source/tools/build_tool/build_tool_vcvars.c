/*==============================================================================================

    build_tool_vcvars.c -- Visual Studio environment discovery and command execution.

    The legacy approach was to prepend "call vcvarsall.bat x64 && " to every
    cl/link/lib invocation. vcvarsall takes 200-500 ms to run, and a real
    engine build issues dozens of compiler calls, so that overhead can easily
    cost 10+ seconds of wall time per build.

    Instead, we now run vcvarsall ONCE at orchestrator startup, capture the
    environment variables it sets via "&& set", and apply them to our own
    process with _putenv_s(). Every child process we spawn after that inherits
    the modified environment for free — no per-invocation prefix needed.

==============================================================================================*/
#include "build_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

/*============================================================================================*/
// --- VC Environment Import ---

// Locate vcvarsall.bat. First try `vswhere` (the modern, future-proof path),
// then fall back to scanning well-known installation paths. Returns true and
// fills `out` with an absolute path on success.
static bool
locate_vcvarsall( char* out, size_t out_size )
{
    const char* vswhere_paths[] = {
        "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
    };

    for ( int i = 0; i < ( int )( sizeof( vswhere_paths ) / sizeof( vswhere_paths[ 0 ] ) ); ++i )
    {
        char cmd[ 1024 ];
        snprintf( cmd, sizeof( cmd ),
                  "%s -latest -products * -property installationPath",
                  vswhere_paths[ i ] );

        FILE* pipe = _popen( cmd, "rt" );
        if ( !pipe ) continue;

        char inst[ 512 ] = { 0 };
        if ( fgets( inst, sizeof( inst ), pipe ) )
        {
            char* nl = strpbrk( inst, "\r\n" );
            if ( nl ) *nl = '\0';
        }
        _pclose( pipe );

        if ( inst[ 0 ] )
        {
            snprintf( out, out_size, "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", inst );
            if ( _access( out, 0 ) == 0 ) return true;
        }
    }

    // Fallback: probe a few common, hard-coded install paths.
    const char* common[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
    };
    for ( int i = 0; i < ( int )( sizeof( common ) / sizeof( common[ 0 ] ) ); ++i )
    {
        if ( _access( common[ i ], 0 ) == 0 )
        {
            snprintf( out, out_size, "%s", common[ i ] );
            return true;
        }
    }

    return false;
}

// Run "<vcvarsall> x64 && set" in a sub-shell and apply every printed
// KEY=VALUE to our own process environment via _putenv_s. Subsequent
// CreateProcess / system children inherit it.
//
// The double-double-quote cmd /c "" "<path>" args "" idiom is required when
// the vcvarsall path itself contains spaces (it always does on default
// installations under "Program Files").
static int
import_vcvars_env( const char* vcvars_path )
{
    char run_cmd[ 1024 ];
    snprintf( run_cmd, sizeof( run_cmd ),
              "cmd /c \"\"%s\" x64 >nul 2>nul && set\"", vcvars_path );

    FILE* pipe = _popen( run_cmd, "rt" );
    if ( !pipe )
    {
        printf( "Warning: could not spawn sub-shell for vcvars import.\n" );
        return 0;
    }

    int imported = 0;
    // Each `set` line may be up to ~8KB (PATH/INCLUDE/LIB get long).
    char line[ 16384 ];
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        size_t l = strlen( line );
        while ( l > 0 && ( line[ l - 1 ] == '\n' || line[ l - 1 ] == '\r' ) ) line[ --l ] = '\0';
        if ( l == 0 ) continue;

        char* eq = strchr( line, '=' );
        if ( !eq || eq == line ) continue;
        *eq = '\0';

        const char* key   = line;
        const char* value = eq + 1;
        if ( _putenv_s( key, value ) == 0 ) ++imported;
    }
    _pclose( pipe );
    return imported;
}

/**
 * build_setup_vc_env()
 *
 * Idempotent one-time setup. If cl.exe is already discoverable in PATH
 * (Developer Command Prompt or pre-sourced vcvars), do nothing. Otherwise
 * locate vcvarsall.bat and import its environment into THIS process so
 * every subsequent child invocation runs with the VC toolchain visible.
 */
void
build_setup_vc_env( void )
{
#if defined( _WIN32 )
    // Fast path: cl.exe already on PATH. system() returns 0 because cl.exe
    // with no args prints its banner and exits 0.
    if ( system( "cl.exe >nul 2>nul" ) == 0 ) return;

    printf( "cl.exe not in PATH. Locating Visual Studio...\n" );

    char vcvars_path[ 512 ] = { 0 };
    if ( !locate_vcvarsall( vcvars_path, sizeof( vcvars_path ) ) )
    {
        printf( "Warning: Could not locate vcvarsall.bat. Compiler calls will fail.\n" );
        return;
    }

    printf( "Importing VC environment from %s\n", vcvars_path );
    int n = import_vcvars_env( vcvars_path );
    printf( "Imported %d environment variables (in-process).\n", n );
#endif
}

/*============================================================================================*/
// --- Command Execution ---

/**
 * build_run_cmd()
 *
 * Run a shell command and return its exit code. The orchestrator's own
 * environment (set up by build_setup_vc_env above) is inherited by the
 * spawned cmd.exe, so cl/link/lib are directly callable with no prefix.
 */
int
build_run_cmd( const char* cmd )
{
    printf( "[CMD] %s\n", cmd );
    return system( cmd );
}

/**
 * build_run_cmd_capture_deps()
 *
 * Same as build_run_cmd, but pipes stdout+stderr through and extracts
 * /showIncludes "Note: including file:" markers into deps_path. Non-marker
 * lines are forwarded to stdout so the user still sees compile warnings
 * and errors.
 *
 * System headers from the MSVC toolchain and Windows SDK are filtered out;
 * they cannot be invalidated by edits to project sources.
 *
 * NOTE: The "Note: including file:" prefix is English-only. For other
 * locales, set VSLANG=1033 in the environment before launching the build.
 */
int
build_run_cmd_capture_deps( const char* cmd, const char* deps_path )
{
    // 2>&1 merges stderr into the pipe so warnings/errors are also forwarded.
    char full_cmd[ CMD_BUF_MAX + 16 ];
    snprintf( full_cmd, sizeof( full_cmd ), "%s 2>&1", cmd );
    printf( "[CMD] %s\n", full_cmd );

    FILE* pipe = _popen( full_cmd, "rt" );
    if ( !pipe ) return -1;

    FILE* deps = deps_path ? fopen( deps_path, "w" ) : NULL;

    static const char   k_prefix[] = "Note: including file:";
    static const size_t k_prefix_n = sizeof( k_prefix ) - 1;

    char line[ 4096 ];
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;

        if ( strncmp( p, k_prefix, k_prefix_n ) == 0 )
        {
            char* path = p + k_prefix_n;
            while ( *path == ' ' || *path == '\t' ) ++path;

            size_t l = strlen( path );
            while ( l > 0 && ( path[ l - 1 ] == '\n' || path[ l - 1 ] == '\r' ) ) path[ --l ] = '\0';
            if ( l == 0 ) continue;

            bool is_system = strstr( path, "\\VC\\Tools\\" ) != NULL
                             || strstr( path, "\\Windows Kits\\" ) != NULL
                             || strstr( path, "/VC/Tools/" ) != NULL
                             || strstr( path, "/Windows Kits/" ) != NULL;

            if ( deps && !is_system ) fprintf( deps, "%s\n", path );
        }
        else
        {
            fputs( line, stdout );
        }
    }

    if ( deps ) fclose( deps );
    return _pclose( pipe );
}
