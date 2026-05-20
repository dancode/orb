/*==============================================================================================

    build_tool.c -- The "Boss" build orchestrator.

==============================================================================================*/

#define _CRT_SECURE_NO_WARNINGS
#include "build_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// --- Command Execution ---

int
build_run_cmd( const char* cmd )
{
    printf( "[CMD] %s\n", cmd );
    return system( cmd );
}

// --- String Buffer (Minimal for building commands) ---

typedef struct
{
    char* buf;
    size_t size;
    size_t cap;
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

// --- Target Logic ---

void
build_clean( void )
{
	 // Deletes the entire bin/ dir and re-creates it as an empty folder.
	 // Ensures old DLLs, PDBs, or EXEs are wiped before a fresh build starts.
	 
    printf( "Cleaning build artifacts...\n" );
#if defined( _WIN32 )
    // Deleting the bin directory on Windows
    build_run_cmd( "rmdir /s /q bin" );
    build_run_cmd( "mkdir bin" );
#else
    build_run_cmd( "rm -rf bin" );
    build_run_cmd( "mkdir bin" );
#endif
    printf( "Clean complete.\n" );
}

bool
build_target( build_context_t* ctx )
{
    cmd_buf_t cmd = { 0 };

    // 1. Pick compiler
    const char* cc = ctx->is_clang ? "clang-cl.exe" : "cl.exe";
    cmd_append( &cmd, "%s ", cc );

    // 2. Common Flags
    cmd_append( &cmd, "/nologo /W4 /WX /Zc:preprocessor /std:c11 " );
    cmd_append( &cmd, "/I source " );

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
    cmd_append( &cmd, "/Fe:bin/sb_base_custom.exe " );
	// $(SolutionDir)..\..\bin\sb_base_custom.exe

    // 6. Linker Flags
    cmd_append( &cmd, "/link /DEBUG " );

    // Execute
    int result = build_run_cmd( cmd.buf );

    free( cmd.buf );
    return result == 0;
}

// --- Main Entry ---

int
main( int argc, char** argv )
{
    bool            should_clean = false;
    build_context_t ctx          = { 0 };
    ctx.config                   = CONFIG_DEBUG;
    ctx.target                   = TARGET_HOST_SANDBOX;

    // Simple arg parsing
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-clean" ) == 0 || strcmp( argv[ i ], "clean" ) == 0 )
            should_clean = true;
        if ( _stricmp( argv[ i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        if ( strcmp( argv[ i ], "clang" ) == 0 ) ctx.is_clang = true;
    }

    if ( should_clean )
    {
        build_clean();
        return 0;
    }

    printf( "--- ORB Build Starting ---\n" );
    printf( "Config: %s\n", ctx.config == CONFIG_DEBUG ? "Debug" : "Release" );
    printf( "Compiler: %s\n", ctx.is_clang ? "Clang" : "MSVC" );

    if ( !build_target( &ctx ) )
    {
        printf( "\nFAILED!\n" );
        return 1;
    }

    printf( "\nSUCCESS!\n" );
    return 0;
}

/*============================================================================================*/