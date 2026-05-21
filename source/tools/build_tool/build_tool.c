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
#include "build_tool_gen.c"

/*============================================================================================*/
// --- Command Execution ---

static char g_vc_env_cmd[ 512 ] = { 0 };

static void
build_setup_vc_env( void )
{
#if defined( _WIN32 )
    // If cl.exe is already in the path, we are good.
    if ( system( "cl.exe >nul 2>nul" ) == 0 ) return;

    printf( "cl.exe not found in PATH. Attempting to locate Visual Studio...\n" );

    // Use vswhere to find the latest VS installation
    // Standard path for vswhere
    const char* vswhere_path = "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"";
    char        cmd[ 512 ];
    sprintf( cmd, "%s -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > vc_path.txt", vswhere_path );

    if ( system( cmd ) == 0 )
    {
        FILE* f = fopen( "vc_path.txt", "r" );
        if ( f )
        {
            char vc_path[ 512 ];
            if ( fgets( vc_path, sizeof( vc_path ), f ) )
            {
                // Remove newline
                char* nl = strchr( vc_path, '\n' );
                if ( nl ) *nl = '\0';
                nl = strchr( vc_path, '\r' );
                if ( nl ) *nl = '\0';

                sprintf( g_vc_env_cmd, "call \"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul && ", vc_path );
                printf( "Found VS at: %s\n", vc_path );
            }
            fclose( f );
            remove( "vc_path.txt" );
        }
    }

    if ( g_vc_env_cmd[ 0 ] == '\0' )
    {
        printf( "Warning: Could not auto-locate Visual Studio. Compiler commands may fail.\n" );
        printf( "Tip: Run from a Developer Command Prompt or check your VS installation.\n" );
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
	 // Deletes the entire bin/ and obj/ dirs and re-creates them as empty folders.
	 // Ensures old DLLs, PDBs, or EXEs are wiped before a fresh build starts.
	 
    printf( "Cleaning build artifacts...\n" );
#if defined( _WIN32 )
    // Deleting directories on Windows
    build_run_cmd( "rmdir /s /q bin" );
    build_run_cmd( "rmdir /s /q obj" );
    build_run_cmd( "mkdir bin" );
    build_run_cmd( "mkdir obj" );
#else
    build_run_cmd( "rm -rf bin obj" );
    build_run_cmd( "mkdir bin obj" );
#endif
    printf( "Clean complete.\n" );
}

/*============================================================================================*/

bool
build_target( build_context_t* ctx )
{
    // Ensure directories exist
#if defined( _WIN32 )
    system( "if not exist bin mkdir bin" );
    system( "if not exist obj mkdir obj" );
#else
    system( "mkdir -p bin obj" );
#endif

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

    // 4. Source Files (Simplified example: just compiling the base test)
	
	if (ctx->target == TARGET_HOST_SANDBOX) {
		// cmd_append(&cmd, "source/base/*.c source/sandbox/sb_base.c ");
	}
   
    cmd_append( &cmd, "source/base/base_main.c " );
	// cmd_append( &cmd, "source/base/base_main.c source/base/base_test.c " );
	
    // 5. Output	
    cmd_append( &cmd, "/Fe:bin/%s.exe ", g_out_name );

    // 6. Linker Flags
    cmd_append( &cmd, "/link /DEBUG /PDB:bin/%s.pdb ", g_out_name );

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
    ctx.target          = TARGET_HOST_SANDBOX;

    bool should_clean   = false;
    bool should_gen     = false;

    // Simple arg parsing
    for ( int i = 1; i < argc; ++i )
    {
        if (  strcmp(  argv[ i ], "-clean" ) == 0 || strcmp( argv[ i ], "clean" ) == 0 ) should_clean = true;
        if (  strcmp(  argv[ i ], "-gen" ) == 0 || strcmp( argv[ i ], "gen" ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        if (  strcmp(  argv[ i ], "clang" ) == 0 ) ctx.is_clang = true;
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
    printf( "\n\n" );

    if ( !build_target( &ctx ) )
    {
        printf( "\nFAILED!\n" );
        return 1;
    }

    printf( "\nSUCCESS!\n" );
    return 0;
}

// clang-format off
/*============================================================================================*/