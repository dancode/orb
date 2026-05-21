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
    if ( g_vc_env_cmd[ 0 ] != '\0' && ( strstr( cmd, "cl.exe" ) || strstr( cmd, "link.exe" ) || strstr( cmd, "lib.exe" ) ) )
    {
        snprintf( full_cmd, sizeof( full_cmd ), "%s %s", g_vc_env_cmd, cmd );
    }
    else
    {
        snprintf( full_cmd, sizeof( full_cmd ), "%s", cmd );
    }

    printf( "[CMD] %s\n", full_cmd );
    return system( full_cmd );
}
