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
static const char* g_build_dir       = "build_new";
static const char* g_int_dir         = "obj";

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
    char cmd[ 256 ];
    sprintf( cmd, "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
    build_run_cmd( "del /s /q bin\\*.pdb >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.lib >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.dll >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.exe >nul 2>nul" ); 
#else
    char cmd[ 256 ];
    sprintf( cmd, "rm -rf bin %s/%s", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
    build_run_cmd( "mkdir bin" );
    sprintf( cmd, "mkdir -p %s/%s", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
#endif
    printf( "Clean complete.\n" );
}

/*============================================================================================*/

bool
build_target( build_context_t* ctx, target_info_t* target )
{
    // 1. Setup paths
    char target_obj_dir[ 256 ];
    sprintf( target_obj_dir, "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );

#if defined( _WIN32 )
    if ( _access( "bin", 0 ) != 0 ) system( "mkdir bin" );
    if ( _access( g_build_dir, 0 ) != 0 )
    {
        char cmd[ 256 ];
        sprintf( cmd, "mkdir %s", g_build_dir );
        system( cmd );
    }
    
    char int_root[ 256 ];
    sprintf( int_root, "%s\\%s", g_build_dir, g_int_dir );
    if ( _access( int_root, 0 ) != 0 )
    {
        char cmd[ 256 ];
        sprintf( cmd, "mkdir %s", int_root );
        system( cmd );
    }

    if ( _access( target_obj_dir, 0 ) != 0 )
    {
        char cmd[ 256 ];
        sprintf( cmd, "mkdir %s", target_obj_dir );
        system( cmd );
    }
#else
    char cmd_mkdir[ 512 ];
    sprintf( cmd_mkdir, "mkdir -p bin %s/%s/%s", g_build_dir, g_int_dir, target->name );
    system( cmd_mkdir );
#endif

    // Self-rebuild protection
    char exe_path[ 256 ];
    sprintf( exe_path, "bin/%s.exe", target->name );
    if ( target->type == TARGET_EXECUTABLE && _access( exe_path, 0 ) == 0 )
    {
        char old_path[ 256 ];
        sprintf( old_path, "bin/%s.exe.old", target->name );
        remove( old_path );
        rename( exe_path, old_path );
    }

    // --- Phase 1: Compile ---
    cmd_buf_t compile_cmd = { 0 };
    const char* cc = ctx->is_clang ? "clang-cl.exe" : "cl.exe";
    cmd_append( &compile_cmd, "%s /c /nologo /W4 /WX /Zc:preprocessor /std:c11 ", cc );
    cmd_append( &compile_cmd, "/I source /Fo%s/ /Fd%s/ ", target_obj_dir, target_obj_dir );

    if ( ctx->config == CONFIG_DEBUG )
        cmd_append( &compile_cmd, "/Zi /Od /MDd /D_DEBUG " );
    else
        cmd_append( &compile_cmd, "/O2 /MD /DNDEBUG " );

    for ( int i = 0; i < target->unit_count; ++i )
        cmd_append( &compile_cmd, "%s/%s ", target->root_dir, target->units[ i ] );

    if ( build_run_cmd( compile_cmd.buf ) != 0 )
    {
        free( compile_cmd.buf );
        return false;
    }
    free( compile_cmd.buf );

    // --- Phase 2: Link/Archive ---
    cmd_buf_t link_cmd = { 0 };
    if ( target->type == TARGET_STATIC_LIB )
    {
        cmd_append( &link_cmd, "lib.exe /nologo /OUT:bin/%s.lib %s/*.obj", target->name, target_obj_dir );
    }
    else
    {
        const char* linker = "link.exe"; // clang-cl usually uses link.exe too, or lld-link
        cmd_append( &link_cmd, "%s /nologo ", linker );
        if ( target->type == TARGET_DYNAMIC_LIB )
            cmd_append( &link_cmd, "/DLL " );

        cmd_append( &link_cmd, "/OUT:bin/%s%s %s/*.obj ", target->name, 
                    (target->type == TARGET_EXECUTABLE) ? ".exe" : ".dll", target_obj_dir );
        
        cmd_append( &link_cmd, "/DEBUG /PDB:bin/%s.pdb ", target->name );

        // Add dependencies
        for ( int i = 0; i < target->dep_count; ++i )
        {
            cmd_append( &link_cmd, "bin/%s.lib ", target->deps[ i ] );
        }
        
        // Add common libraries
        cmd_append( &link_cmd, "user32.lib shell32.lib gdi32.lib advapi32.lib " );

        // If this is an EXE or DLL that needs orb_base, we'd add it here.
        // For now, we assume unity builds include everything needed.
    }

    int result = build_run_cmd( link_cmd.buf );
    free( link_cmd.buf );

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