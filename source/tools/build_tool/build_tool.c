/*==============================================================================================

    build_tool.c -- The "Boss" build orchestrator.

==============================================================================================*/
// clang-format off

#include "build_tool.h"

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>

static const char* g_proj_name       = "orb_make";
static const char* g_out_name        = "sb_base_custom";
static const char* g_build_proj_name = "orb_build";

// Unity Includes
#include "build_tool_targets.c"
#include "build_tool_gen.c"

/*============================================================================================*/
// --- Command Execution ---

static char g_vc_env_cmd[ 512 ] = { 0 };

static void
build_setup_vc_env( void )
{
#if defined( _WIN32 )
    if ( system( "cl.exe >nul 2>nul" ) == 0 ) return;

    printf( "cl.exe not found in PATH. Attempting to locate Visual Studio...\n" );

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

int
build_run_cmd( const char* cmd )
{
    char full_cmd[ 8192 ];
    if ( g_vc_env_cmd[ 0 ] != '\0' && ( strstr( cmd, "cl.exe" ) || strstr( cmd, "link.exe" ) ) )
    {
        sprintf( full_cmd, "%s %s", g_vc_env_cmd, cmd );
    }
    else
    {
        strcpy( full_cmd, cmd );
    }

    printf( "[CMD] %s\n", full_cmd );
    return system( full_cmd );
}

/*============================================================================================*/
// --- String Buffer (Minimal for building commands) ---

typedef struct
{
    char*   buf;
    size_t  size;
    size_t  cap;

} cmd_buf_t;

void
cmd_append( cmd_buf_t* b, const char* fmt, ... )
{
    if ( b->buf == NULL )
    {
        b->cap = 4096;
        b->buf = malloc( b->cap );
        b->buf[ 0 ] = '\0';
    }

    va_list args;
    va_start( args, fmt );
    char tmp[ 4096 ];
    vsnprintf( tmp, sizeof( tmp ), fmt, args );
    va_end( args );

    size_t len = strlen( tmp );
    if ( b->size + len + 1 >= b->cap )
    {
        b->cap *= 2;
        b->buf = realloc( b->buf, b->cap );
    }

    strcat( b->buf, tmp );
    b->size += len;
}

/*============================================================================================*/
// --- Target Logic ---

void
build_clean( void )
{
    printf( "Cleaning build artifacts...\n" );
#if defined( _WIN32 )
    // We avoid rmdir /s /q bin because the build_tool.exe itself is likely running from there.
    // Instead, we surgically delete files we can, and ignore the rest.
    build_run_cmd( "del /s /q obj\\* >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.pdb >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.lib >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.dll >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.exe >nul 2>nul" ); 
#else
    build_run_cmd( "rm -rf bin obj" );
    build_run_cmd( "mkdir bin obj" );
#endif
    printf( "Clean complete.\n" );
}

/*============================================================================================*/

bool
build_target( build_context_t* ctx, target_info_t* target )
{
    // Ensure directories exist
#if defined( _WIN32 )
    if ( _access( "bin", 0 ) != 0 ) system( "mkdir bin" );
    if ( _access( "obj", 0 ) != 0 ) system( "mkdir obj" );
#else
    system( "mkdir -p bin obj" );
#endif

    // Self-rebuild protection: If we are building ourselves, rename the running exe
    // so the linker can create a new one without "Access Denied".
    char exe_path[ 256 ];
    sprintf( exe_path, "bin/%s.exe", target->name );
    if ( target->type == TARGET_EXECUTABLE && _access( exe_path, 0 ) == 0 )
    {
        char old_path[ 256 ];
        sprintf( old_path, "bin/%s.exe.old", target->name );
        remove( old_path );
        if ( rename( exe_path, old_path ) != 0 )
        {
            // If rename fails, it might be already renamed or locked by something else.
            // We continue anyway and let the linker report the error if it persists.
        }
    }

    cmd_buf_t cmd = { 0 };

    // 1. Pick compiler
    const char* cc = ctx->is_clang ? "clang-cl.exe" : "cl.exe";
    cmd_append( &cmd, "%s ", cc );

    // 2. Common Flags  
    cmd_append( &cmd, "/nologo /W4 /WX /Zc:preprocessor /std:c11 " );
    cmd_append( &cmd, "/I source " );
    
    // Intermediate files
    cmd_append( &cmd, "/Foobj/ /Fdobj/ " );

    // 3. Config Flags
    if ( ctx->config == CONFIG_DEBUG )
    {
        cmd_append( &cmd, "/Zi /Od /MDd /D_DEBUG " );
    }
    else
    {
        cmd_append( &cmd, "/O2 /MD /DNDEBUG " );
    }

    // 4. Source Files (Target Units)
    for ( int i = 0; i < target->unit_count; ++i )
    {
        cmd_append( &cmd, "%s/%s ", target->root_dir, target->units[ i ] );
    }
    
    // 5. Output
    if ( target->type == TARGET_STATIC_LIB )
    {
        cmd_append( &cmd, "/c /Foobj/%s.obj ", target->name );
        // NOTE: Static libraries require a separate link step (lib.exe)
        // We'll just compile to obj for now or add lib.exe support.
    }
    else
    {
        cmd_append( &cmd, "/Fe:bin/%s.exe ", target->name );
        // 6. Linker Flags
        cmd_append( &cmd, "/link /DEBUG /PDB:bin/%s.pdb ", target->name );
    }

    // Execute
    int result = build_run_cmd( cmd.buf );

    free( cmd.buf );
    return result == 0;
}

/*============================================================================================*/
// --- Main Entry ---

int
main( int argc, char** argv )
{
    build_context_t ctx = { 0 };

    ctx.config          = CONFIG_DEBUG;

    bool should_clean   = false;
    bool should_gen     = false;
    char* target_name   = NULL;

    // Simple arg parsing
    for ( int i = 1; i < argc; ++i )
    {
        if (  strcmp(  argv[ i ], "-clean" ) == 0 || strcmp( argv[ i ], "clean" ) == 0 ) should_clean = true;
        if (  strcmp(  argv[ i ], "-gen" ) == 0 || strcmp( argv[ i ], "gen" ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        if (  strcmp(  argv[ i ], "clang" ) == 0 ) ctx.is_clang = true;
        if (  strcmp(  argv[ i ], "-target" ) == 0 && i + 1 < argc ) target_name = argv[ ++i ];
    }

    if ( should_clean )
    {
        build_clean();
        return 0;
    }

    if ( should_gen )
    {
        build_gen_projects();
        return 0;
    }

    printf( "--- ORB Build Starting ---\n\n" );

    build_setup_vc_env();

    printf( "Config: %s\n", ctx.config == CONFIG_DEBUG ? "Debug" : "Release" );
    printf( "Compiler: %s\n", ctx.is_clang ? "Clang" : "MSVC" );
    printf( "\n" );

    if ( target_name )
    {
        target_info_t* target = NULL;
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( _stricmp( g_targets[ i ].name, target_name ) == 0 )
            {
                target = &g_targets[ i ];
                break;
            }
        }

        if ( target )
        {
            if ( !build_target( &ctx, target ) )
            {
                printf( "\nFAILED!\n" );
                return 1;
            }
        }
        else
        {
            printf( "Error: Unknown target '%s'\n", target_name );
            return 1;
        }
    }
    else
    {
        // Build all targets if none specified
        for ( int i = 0; i < g_target_count; ++i )
        {
            printf( "Building target: %s\n", g_targets[ i ].name );
            if ( !build_target( &ctx, &g_targets[ i ] ) )
            {
                printf( "\nFAILED on target '%s'!\n", g_targets[ i ].name );
                return 1;
            }
        }
    }

    printf( "\nSUCCESS!\n" );
    return 0;
}

// clang-format off
/*============================================================================================*/