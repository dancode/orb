/*==============================================================================================

    build_tool_vcvars.c -- Visual Studio environment discovery and command execution.

==============================================================================================*/
#include "build_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

/*============================================================================================*/
// --- Command Execution & Environment ---

// Stores the "call vcvarsall.bat &&" prefix used for all compiler commands.
static char g_vc_env_cmd[ 512 ] = { 0 };

/**
 * build_setup_vc_env()
 * 
 * Locates the Visual Studio installation on the host machine. 
 * This is crucial because cl.exe and link.exe are not in the PATH by default.
 */
void
build_setup_vc_env( void )
{
#if defined( _WIN32 )
    // Optimization: If cl.exe is already in the PATH (e.g. running from a Dev Cmd Prompt),
    // we don't need to do anything.
    if ( system( "cl.exe >nul 2>nul" ) == 0 ) return;

    printf( "cl.exe not found in PATH. Attempting to locate Visual Studio...\n" );

    // Common locations for vswhere.exe (the standard VS discovery tool).
    const char* vswhere_paths[] = {
        "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",        
        "\"%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
    };

    bool found = false;
    for ( int i = 0; i < sizeof( vswhere_paths ) / sizeof( vswhere_paths[ 0 ] ); ++i )
    {
        char cmd[ 1024 ];
        sprintf( cmd, "%s -latest -products * -property installationPath > vc_path.txt", vswhere_paths[ i ] );

        if ( system( cmd ) == 0 )
        {
            FILE* f = fopen( "vc_path.txt", "r" );
            if ( f )
            {
                char vc_path[ 512 ];
                if ( fgets( vc_path, sizeof( vc_path ), f ) )
                {
                    char* nl = strpbrk( vc_path, "\r\n" );
                    if ( nl ) *nl = '\0';

                    if ( strlen( vc_path ) > 0 )
                    {
                        sprintf( g_vc_env_cmd, "call \"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul && ", vc_path );
                        found = true;
                    }
                }
                fclose( f );
                remove( "vc_path.txt" );
            }
        }
        if ( found ) break;
    }

    if ( found )
    {
        printf( "VC Environment setup command: %s\n", g_vc_env_cmd );
    }
    else
    {
        // Fallback: Check common installation paths directly.
        printf( "Warning: Could not auto-locate Visual Studio via vswhere. Trying common paths...\n" );
        const char* common_vcvars[] = {
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        };

        for ( int i = 0; i < sizeof( common_vcvars ) / sizeof( common_vcvars[ 0 ] ); ++i )
        {
            if ( _access( common_vcvars[ i ], 0 ) == 0 )
            {
                sprintf( g_vc_env_cmd, "call \"%s\" x64 >nul && ", common_vcvars[ i ] );
                printf( "Found vcvarsall.bat at: %s\n", common_vcvars[ i ] );
                found = true;
                break;
            }
        }
    }

    if ( !found )
    {
        printf( "Warning: Could not locate Visual Studio. Compiler commands will likely fail.\n" );
    }
#endif
}

// Compose the final command line, prepending the VC env if this is a tool call.
static void
build_compose_full_cmd( const char* cmd, const char* suffix, char* out, size_t out_size )
{
    bool needs_env = ( g_vc_env_cmd[ 0 ] != '\0' )
                     && ( strstr( cmd, "cl.exe" ) || strstr( cmd, "link.exe" ) || strstr( cmd, "lib.exe" ) );

    if ( needs_env )
        snprintf( out, out_size, "%s %s%s", g_vc_env_cmd, cmd, suffix ? suffix : "" );
    else
        snprintf( out, out_size, "%s%s", cmd, suffix ? suffix : "" );
}

/**
 * build_run_cmd()
 *
 * A wrapper around system() that automatically injects the VC environment prefix
 * if the command is a compiler/linker call.
 */
int
build_run_cmd( const char* cmd )
{
    char full_cmd[ CMD_BUF_MAX + 1024 ];
    build_compose_full_cmd( cmd, NULL, full_cmd, sizeof( full_cmd ) );
    printf( "[CMD] %s\n", full_cmd );
    return system( full_cmd );
}

/**
 * build_run_cmd_capture_deps()
 *
 * Variant of build_run_cmd that captures the compiler's stdout, parses
 * /showIncludes "Note: including file:" markers, and writes the resulting
 * header paths to deps_path (one per line). Lines that are not include
 * markers are forwarded to stdout so compile errors and warnings remain
 * visible to the user.
 *
 * System headers from the MSVC toolchain and Windows SDK are filtered out;
 * they cannot be invalidated by edits to project sources, so tracking them
 * just bloats the dep file and wastes stat() calls on incremental builds.
 *
 * NOTE: The "Note: including file:" prefix is locale-dependent. This works
 * on English-locale MSVC. For other locales, set the cl flag
 * /D_CL_SHOWINCLUDES_ENGLISH or pipe through VSLANG=1033 in the env.
 */
int
build_run_cmd_capture_deps( const char* cmd, const char* deps_path )
{
    char full_cmd[ CMD_BUF_MAX + 1024 ];
    // 2>&1 merges stderr into the pipe so warnings/errors are also forwarded.
    build_compose_full_cmd( cmd, " 2>&1", full_cmd, sizeof( full_cmd ) );
    printf( "[CMD] %s\n", full_cmd );

    FILE* pipe = _popen( full_cmd, "rt" );
    if ( !pipe ) return -1;

    FILE* deps = deps_path ? fopen( deps_path, "w" ) : NULL;

    static const char k_prefix[]   = "Note: including file:";
    static const size_t k_prefix_n = sizeof( k_prefix ) - 1;

    char line[ 4096 ];
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        // /showIncludes indents the marker with one space per include depth.
        char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;

        if ( strncmp( p, k_prefix, k_prefix_n ) == 0 )
        {
            char* path = p + k_prefix_n;
            while ( *path == ' ' || *path == '\t' ) ++path;

            // Strip CR/LF.
            size_t l = strlen( path );
            while ( l > 0 && ( path[ l - 1 ] == '\n' || path[ l - 1 ] == '\r' ) ) path[ --l ] = '\0';
            if ( l == 0 ) continue;

            // Filter system headers — they're owned by the toolchain, not us.
            bool is_system = strstr( path, "\\VC\\Tools\\" ) != NULL
                             || strstr( path, "\\Windows Kits\\" ) != NULL
                             || strstr( path, "/VC/Tools/" ) != NULL
                             || strstr( path, "/Windows Kits/" ) != NULL;

            if ( deps && !is_system ) fprintf( deps, "%s\n", path );
        }
        else
        {
            // Forward normal compiler output so the user sees source banners,
            // warnings, and errors as usual.
            fputs( line, stdout );
        }
    }

    if ( deps ) fclose( deps );
    return _pclose( pipe );
}
