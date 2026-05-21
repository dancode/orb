/*==============================================================================================

    build_tool.c -- The "Boss" build orchestrator.

    This tool is the heart of ORB's custom build system. It replaces complex Makefiles
    or CMake scripts with a simple, high-performance C program that directly invokes
    the compiler (cl.exe) and linker (link.exe).

    Key Responsibilities:
    1. Locating Visual Studio and setting up the shell environment (vcvarsall).
    2. Managing build artifacts (bin/ and obj/ directories).
    3. Coordinating reflection generation via build_reflect.exe.
    4. Constructing and executing compiler/linker command lines for all targets.
    5. Generating Visual Studio solution files for developer ergonomics.

==============================================================================================*/
// clang-format off

#include "build_tool.h"

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <io.h>

// --- Project Constants ---

static const char* g_proj_name       = "orb_make";       // The main VS solution name.
static const char* g_out_name        = "sb_base_custom"; // Default "output" target for navigation.
static const char* g_build_proj_name = "orb_build";      // Secondary build solution.
static const char* g_build_dir       = "build_new";      // Root for intermediate/generated files.
static const char* g_int_dir         = "obj";            // Folder for .obj files.
static const char* g_gen_dir         = "generated";      // Folder for reflection-generated code.

// --- Unity Includes ---
// We include the target registry and project generator directly to keep the
// build tool as a single compilation unit for extreme speed and simplicity.
#include "build_tool_targets.c"
#include "build_tool_gen.c"

/*============================================================================================*/
// --- Command Execution & Environment ---

#define CMD_BUF_MAX 65536  // Max size for any single compiler/linker command line.

// Stores the "call vcvarsall.bat &&" prefix used for all compiler commands.
static char g_vc_env_cmd[ 512 ] = { 0 };

/**
 * build_setup_vc_env()
 * 
 * Locates the Visual Studio installation on the host machine. 
 * This is crucial because cl.exe and link.exe are not in the PATH by default.
 * It uses 'vswhere.exe' to find the latest VS installation and prepares a 
 * "call vcvarsall.bat" command that will be prefixed to every compiler call.
 */
static void
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
        // Run vswhere to get the installation path and pipe it to a temporary file.
        sprintf( cmd, "%s -latest -products * -property installationPath > vc_path.txt", vswhere_paths[ i ] );

        if ( system( cmd ) == 0 )
        {
            FILE* f = fopen( "vc_path.txt", "r" );
            if ( f )
            {
                char vc_path[ 512 ];
                if ( fgets( vc_path, sizeof( vc_path ), f ) )
                {
                    // Clean up newline characters from the path.
                    char* nl = strpbrk( vc_path, "\r\n" );
                    if ( nl ) *nl = '\0';

                    if ( strlen( vc_path ) > 0 )
                    {
                        // Construct the command that sets up the x64 environment.
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
        // Fallback: If vswhere fails, check common installation paths directly.
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
    // Extra 1024 for the vcvarsall prefix on top of the max command length.
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

/*============================================================================================*/
// --- String Buffer (Helper for building command lines) ---

typedef struct
{
    char   buf[ CMD_BUF_MAX ];
    size_t size;

} cmd_buf_t;

static void
cmd_append( cmd_buf_t* b, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( b->buf + b->size, CMD_BUF_MAX - b->size, fmt, args );
    va_end( args );
    if ( written > 0 ) b->size += (size_t)written;
}

/*============================================================================================*/
// --- Core Build Logic ---

/**
 * build_clean()
 * 
 * Wipes the bin/ and obj/ directories. 
 * Special care is taken on Windows to not delete the build_tool.exe itself 
 * because it's usually the one running this code!
 */
void
build_clean( void )
{
    printf( "Cleaning build artifacts...\n" );
#if defined( _WIN32 )
    char cmd[ 256 ];
    // Delete intermediate objects and generated reflection code.
    sprintf( cmd, "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
    sprintf( cmd, "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_gen_dir );
    build_run_cmd( cmd );
    
    // Delete binaries.
    build_run_cmd( "del /s /q bin\\*.pdb >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.lib >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.dll >nul 2>nul" );

    // Surgical delete: remove all EXEs EXCEPT ourselves. 
    // This allows the build system to "self-clean" without crashing.
    build_run_cmd( "for %f in (bin\\*.exe) do if not \"%~nxf\"==\"build_tool.exe\" del \"%f\" >nul 2>nul" );
#else
    char cmd[ 256 ];
    sprintf( cmd, "rm -rf bin %s/%s %s/%s", g_build_dir, g_int_dir, g_build_dir, g_gen_dir );
    build_run_cmd( cmd );
    build_run_cmd( "mkdir bin" );
    sprintf( cmd, "mkdir -p %s/%s", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
    sprintf( cmd, "mkdir -p %s/%s", g_build_dir, g_gen_dir );
    build_run_cmd( cmd );
#endif
    printf( "Clean complete.\n" );
}

/*============================================================================================*/

/**
 * build_target()
 * 
 * The main worker function for building a single artifact.
 * 
 * Orchestration Flow:
 * 1. Directory Setup: Creates bin/, obj/, and generated/ folders for the target.
 * 2. Self-Rebuild Protection: Renames the existing .exe/.pdb if they are locked.
 * 3. Reflection: Runs build_reflect.exe if required by the target.
 * 4. Phase 1 (Compile): Constructs and runs a cl.exe command for all .c files.
 * 5. Phase 2 (Link/Archive): Constructs and runs link.exe or lib.exe.
 */
bool
build_target( build_context_t* ctx, target_info_t* target )
{
    // --- 1. Path Preparation ---
    char target_obj_dir[ 256 ];
    sprintf( target_obj_dir, "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
    char target_gen_dir[ 256 ];
    sprintf( target_gen_dir, "%s\\%s", g_build_dir, g_gen_dir );

    char exe_path[ 256 ];
    char old_path[ 256 ];
    bool renamed = false;
    sprintf( exe_path, "bin\\%s.exe", target->name );
    sprintf( old_path, "bin\\%s.exe.old", target->name );

#if defined( _WIN32 )
    // Ensure the entire directory hierarchy exists before starting.
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

    if ( _access( target_gen_dir, 0 ) != 0 )
    {
        char cmd[ 256 ];
        sprintf( cmd, "mkdir %s", target_gen_dir );
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
    sprintf( cmd_mkdir, "mkdir -p bin %s/%s/%s %s/%s", g_build_dir, g_int_dir, target->name, g_build_dir, g_gen_dir );
    system( cmd_mkdir );
#endif

    // --- 2. Locked File Management ---

    // Self-rebuild protection: On Windows, you can't overwrite a running .exe, 
    // but you CAN rename it. This allows the build_tool to rebuild itself 
    // while it's running.
    if ( target->type == TARGET_EXECUTABLE && _access( exe_path, 0 ) == 0 )
    {
        remove( old_path );
        if ( rename( exe_path, old_path ) == 0 )
        {
            renamed = true;
        }
    }

    // PDB Unlock: Simlar to EXEs, debuggers (like VS) often lock .pdb files.
    // Renaming them allows the linker to write a fresh one.
    if ( target->type == TARGET_EXECUTABLE || target->type == TARGET_DYNAMIC_LIB )
    {
        char pdb_path[ 256 ];
        sprintf( pdb_path, "bin/%s.pdb", target->name );
        if ( _access( pdb_path, 0 ) == 0 )
        {
            char old_pdb[ 256 ];
            sprintf( old_pdb, "bin/%s.pdb.old", target->name );
            remove( old_pdb );
            rename( pdb_path, old_pdb );
        }
    }

    // --- 3. Reflection Generation ---

    // If the target has the has_reflect flag, we invoke the specialized
    // build_reflect.exe tool. It parses the target's source code and
    // generates metadata (.c/.h) files used by the engine's runtime 
    // reflection system.
    if ( target->has_reflect )
    {
        printf( "[REFL] Generating reflection for %s...\n", target->reflect_name );
        char refl_cmd[ 1024 ];
        sprintf( refl_cmd, "bin\\build_reflect.exe %s %s %s", target->root_dir, target_gen_dir, target->reflect_name );
        if ( build_run_cmd( refl_cmd ) != 0 )
        {
            // If reflection fails, we abort and try to restore the old binary.
            if ( renamed ) rename( old_path, exe_path );
            printf( "Error: Reflection generation failed for %s\n", target->name );
            return false;
        }
    }

    // --- Phase 1: Compile ---

    cmd_buf_t compile_cmd = { 0 };
    const char* cc = ctx->is_clang ? "clang-cl.exe" : "cl.exe";

    // Base flags: 
    // /c: Compile only, no link.
    // /nologo: Suppress banner.
    // /W4 /WX: Max warnings, treat as errors.
    // /std:c11: Use C11 standard.
    cmd_append( &compile_cmd, "%s /c /nologo /W4 /WX /Zc:preprocessor /std:c11 ", cc );
    
    // Include paths and output directories.
    cmd_append( &compile_cmd, "/I source /I %s /Fo%s/ /Fd%s/ ", target_gen_dir, target_obj_dir, target_obj_dir );

    // Architectural Defines used globally in the engine.
    cmd_append( &compile_cmd, "/DOS_WINDOWS /DCOMPILER_MSVC /DARCH_X64 /D_CRT_SECURE_NO_WARNINGS " );

    // Target-specific static define (e.g. /DBASE_STATIC).
    // This is used for export/import macros in headers.
    char target_upper[ 128 ];
    strcpy( target_upper, target->name );
    for ( char* p = target_upper; *p; ++p ) *p = ( char )toupper( *p );
    cmd_append( &compile_cmd, "/D%s_STATIC ", target_upper );

    if ( ctx->is_monolithic )
        cmd_append( &compile_cmd, "/DBUILD_STATIC " );

    // Config-specific flags.
    if ( ctx->config == CONFIG_DEBUG )
        cmd_append( &compile_cmd, "/Zi /Od /MDd /D_DEBUG " ); // Debug: Symbols, No Opts, Debug CRT.
    else
        cmd_append( &compile_cmd, "/O2 /MD /DNDEBUG " );      // Release: Max Opts, No Symbols, Release CRT.

    // Add all translation units listed in the target descriptor.
    for ( int i = 0; i < target->unit_count; ++i )
        cmd_append( &compile_cmd, "%s/%s ", target->root_dir, target->units[ i ] );

    // Add generated reflection code if applicable.
    if ( target->has_reflect )
    {
        cmd_append( &compile_cmd, "%s/%s.generated.c ", target_gen_dir, target->reflect_name );
    }

    if ( build_run_cmd( compile_cmd.buf ) != 0 )
    {
        if ( renamed ) rename( old_path, exe_path );
        return false;
    }

    // --- Phase 2: Link/Archive ---

    cmd_buf_t link_cmd = { 0 };
    if ( target->type == TARGET_STATIC_LIB )
    {
        // For static libraries, we use lib.exe to bundle .obj files into a .lib archive.
        cmd_append( &link_cmd, "lib.exe /nologo /OUT:bin/%s.lib %s/*.obj", target->name, target_obj_dir );
    }
    else
    {
        // For EXEs and DLLs, we use link.exe.
        const char* linker = "link.exe";
        cmd_append( &link_cmd, "%s /nologo ", linker );
        
        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            cmd_append( &link_cmd, "/DLL " );
            // Import lib lands in bin/ alongside static libs so dep resolution is uniform.
            cmd_append( &link_cmd, "/IMPLIB:bin/%s.lib ", target->name );
        }

        cmd_append( &link_cmd, "/OUT:bin/%s%s %s/*.obj ", target->name, 
                    (target->type == TARGET_EXECUTABLE) ? ".exe" : ".dll", target_obj_dir );
        
        cmd_append( &link_cmd, "/DEBUG /PDB:bin/%s.pdb ", target->name );

        // Link against dependencies listed in the registry.
        // For now, we assume all dependencies are static libraries located in bin/.
        for ( int i = 0; i < target->dep_count; ++i )
        {
            cmd_append( &link_cmd, "bin/%s.lib ", target->deps[ i ] );
        }
        
        // Link against standard Windows system libraries.
        cmd_append( &link_cmd, "user32.lib shell32.lib gdi32.lib advapi32.lib " );
    }

    int result = build_run_cmd( link_cmd.buf );

    if ( result != 0 && renamed )
    {
        // If the build failed, try to restore the old executable so the user
        // still has something they can run (or so build_tool.exe is preserved).
        rename( old_path, exe_path );
    }

    return result == 0;
}

/*============================================================================================*/
// --- Main Entry ---

/**
 * main()
 * 
 * Orchestrates the overall build process based on command line arguments.
 */
int
main( int argc, char** argv )
{
    build_context_t ctx = { 0 };

    ctx.config          = CONFIG_DEBUG; // Default to Debug.

    bool should_clean   = false;
    bool should_gen     = false;
    char* target_name   = NULL;

    // --- 1. Argument Parsing ---
    for ( int i = 1; i < argc; ++i )
    {
        if (  strcmp(  argv[ i ], "-clean" ) == 0 || strcmp( argv[ i ], "clean" ) == 0 ) should_clean = true;
        if (  strcmp(  argv[ i ], "-gen" ) == 0 || strcmp( argv[ i ], "gen" ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        if (  strcmp(  argv[ i ], "clang" ) == 0 ) ctx.is_clang = true;
        if (  strcmp(  argv[ i ], "-target" ) == 0 && i + 1 < argc ) target_name = argv[ ++i ];
        // VS NMake passes -config Debug|Release — this is the primary config selector from the IDE.
        if (  strcmp(  argv[ i ], "-config" ) == 0 && i + 1 < argc )
        {
            if ( _stricmp( argv[ ++i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        }
    }

    // --- 2. Clean Command ---
    if ( should_clean )
    {
        build_clean();
        return 0;
    }

    // --- 3. Project Generation ---
    if ( should_gen )
    {
        build_gen_projects();
        return 0;
    }

    printf( "--- ORB Build Starting ---\n\n" );

    // --- 4. Environment Setup ---
    build_setup_vc_env();

    printf( "Config: %s\n", ctx.config == CONFIG_DEBUG ? "Debug" : "Release" );
    printf( "Compiler: %s\n", ctx.is_clang ? "Clang" : "MSVC" );
    printf( "\n" );

    // --- 5. Bootstrapping (Reflection Tool) ---

    // The reflection tool is a dependency for many other targets.
    // We try to build it first every time to ensure the generator is up to date.
    target_info_t* refl_tool = NULL;
    for ( int i = 0; i < g_target_count; ++i )
    {
        if ( strcmp( g_targets[ i ].name, "build_reflect" ) == 0 )
        {
            refl_tool = &g_targets[ i ];
            break;
        }
    }
    if ( refl_tool )
    {
        if ( !build_target( &ctx, refl_tool ) )
        {
            printf( "Error: Failed to build reflection tool!\n" );
            return 1;
        }
    }

    // --- 6. Target Execution ---

    if ( target_name )
    {
        // Build a specific target requested by the user.
        target_info_t* target = NULL;
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( _stricmp( g_targets[ i ].name, target_name ) == 0 )
            {
                if ( &g_targets[ i ] == refl_tool ) return 0; // Already built.
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
        // Default behavior: Build all targets in the registry sequentially.
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

// clang-format on
/*============================================================================================*/