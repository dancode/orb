/*==============================================================================================

    build_tool.c -- The "Boss" build orchestrator.

    This tool is the heart of ORB's custom build system. It replaces complex Makefiles
    or CMake scripts with a simple, high-performance C program that directly invokes
    the compiler (cl.exe) and linker (link.exe).

    Architecture Note:
    This tool is designed to be a "Unity Build" — all supporting files (.c) are 
    directly included here. This makes bootstrapping the build tool itself 
    instantaneous (see bootstrap_build_tool.bat).

==============================================================================================*/
// clang-format off

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "build_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <io.h>
#include <sys/stat.h>
#include <time.h>

// --- Project Constants ---

static const char* g_build_dir       = "build_new";      // Root for intermediate/generated files.
static const char* g_int_dir         = "obj";            // Folder for .obj files.
static const char* g_gen_dir         = "generated";      // Folder for reflection-generated code.

// --- Unity Includes ---
#include "build_tool_utils.c"
#include "build_tool_vcvars.c"
#include "build_tool_cc.c"
#include "build_tool_targets.c"
#include "build_tool_gen.c"
#include "build_tool_sched.c"

/*============================================================================================*/
// --- Core Build Logic ---

/**
 * build_clean()
 * 
 * Wipes the bin/ and obj/ directories. 
 */
void
build_clean( void )
{
    printf( "Cleaning build artifacts...\n" );
#if defined( _WIN32 )
    char cmd[ BT_PATH_MAX ];
    snprintf( cmd, sizeof( cmd ), "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
    snprintf( cmd, sizeof( cmd ), "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_gen_dir );
    build_run_cmd( cmd );

    build_run_cmd( "del /s /q bin\\*.pdb >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.lib >nul 2>nul" );
    build_run_cmd( "del /s /q bin\\*.dll >nul 2>nul" );

    // Surgical delete: remove all EXEs EXCEPT ourselves.
    build_run_cmd( "for %f in (bin\\*.exe) do if not \"%~nxf\"==\"build_tool.exe\" del \"%f\" >nul 2>nul" );
#else
    char cmd[ BT_PATH_MAX ];
    snprintf( cmd, sizeof( cmd ), "rm -rf bin %s/%s %s/%s", g_build_dir, g_int_dir, g_build_dir, g_gen_dir );
    build_run_cmd( cmd );
    build_run_cmd( "mkdir bin" );
    snprintf( cmd, sizeof( cmd ), "mkdir -p %s/%s", g_build_dir, g_int_dir );
    build_run_cmd( cmd );
    snprintf( cmd, sizeof( cmd ), "mkdir -p %s/%s", g_build_dir, g_gen_dir );
    build_run_cmd( cmd );
#endif
    printf( "Clean complete.\n" );
}

/*============================================================================================*/

/**
 * build_target()
 * 
 * The main worker function for building a single artifact.
 */
bool
build_target( build_context_t* ctx, target_info_t* target )
{
    // --- 0. Dependency Resolution ---
    //
    // Skipped when -no-deps is set. The VS solution generator emits that flag
    // because MSBuild's own scheduler honors ProjectDependencies and will
    // launch dep projects before dependent ones — letting the orchestrator
    // also recurse would mean every dep gets walked once per dependent and
    // multiple build_tool.exe processes would race on shared outputs.

    if ( !ctx->skip_deps )
    {
        // Link Dependencies (libs)
        for ( int i = 0; i < target->dep_count; ++i )
        {
            target_info_t* dep = NULL;
            for ( int j = 0; j < g_target_count; ++j )
            {
                if ( strcmp( g_targets[ j ].name, target->deps[ i ] ) == 0 ) { dep = &g_targets[ j ]; break; }
            }
            if ( dep && !build_target( ctx, dep ) ) return false;
        }

        // Tool Dependencies (exes)
        for ( int i = 0; i < target->tool_dep_count; ++i )
        {
            target_info_t* tool = NULL;
            for ( int j = 0; j < g_target_count; ++j )
            {
                if ( strcmp( g_targets[ j ].name, target->tool_deps[ i ] ) == 0 ) { tool = &g_targets[ j ]; break; }
            }
            if ( tool && !build_target( ctx, tool ) ) return false;
        }
    }

    // --- Critical section ---
    //
    // Hold a per-target named mutex from path prep through link. Two concurrent
    // build_tool.exe invocations of the SAME target will serialize here; two
    // invocations of independent targets run in parallel (different mutex
    // names). Acquired BEFORE the up-to-date check so a second invocation
    // observes the post-build artifact mtimes — never a half-written .obj/.lib.

    void* target_lock = build_lock_target( target->name );
    bool  result      = true;

    // --- 1. Path Preparation ---
    char obj_dir[ BT_PATH_MAX ];
    snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
    char gen_dir[ BT_PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );

    const char* ext = ( target->type == TARGET_STATIC_LIB )  ? ".lib" :
                      ( target->type == TARGET_DYNAMIC_LIB ) ? ".dll" : ".exe";

    char out_path[ BT_PATH_MAX ];
    snprintf( out_path, sizeof( out_path ), "bin\\%s%s", target->name, ext );

    // --- 2. Up-to-Date Check ---

    __time64_t out_mtime = build_get_mtime( out_path );
    bool up_to_date = ( out_mtime != 0 );

    if ( up_to_date )
    {
        for ( int i = 0; i < target->unit_count; ++i )
        {
            char src_path[ BT_PATH_MAX ];
            snprintf( src_path, sizeof( src_path ), "%s/%s", target->root_dir, target->units[ i ] );
            if ( build_get_mtime( src_path ) > out_mtime ) { up_to_date = false; break; }
        }
    }

    if ( up_to_date )
    {
        for ( int i = 0; i < target->dep_count; ++i )
        {
            char dep_path[ BT_PATH_MAX ];
            snprintf( dep_path, sizeof( dep_path ), "bin\\%s.lib", target->deps[ i ] );
            if ( build_get_mtime( dep_path ) > out_mtime ) { up_to_date = false; break; }
        }
    }

    // Header dependency check. _deps.txt is written by /showIncludes during
    // the previous compile (see build_target_compile). If it does not exist,
    // we have no header info → force a rebuild. If any listed header is newer
    // than the artifact, rebuild.
    if ( up_to_date )
    {
        char deps_path[ BT_PATH_MAX ];
        snprintf( deps_path, sizeof( deps_path ), "%s\\_deps.txt", obj_dir );
        FILE* deps = fopen( deps_path, "r" );
        if ( !deps )
        {
            up_to_date = false;
        }
        else
        {
            char header_path[ BT_PATH_MAX ];
            while ( fgets( header_path, sizeof( header_path ), deps ) )
            {
                size_t l = strlen( header_path );
                while ( l > 0 && ( header_path[ l - 1 ] == '\n' || header_path[ l - 1 ] == '\r' ) )
                    header_path[ --l ] = '\0';
                if ( l == 0 ) continue;

                if ( build_get_mtime( header_path ) > out_mtime )
                {
                    up_to_date = false;
                    break;
                }
            }
            fclose( deps );
        }
    }

    if ( up_to_date ) { result = true; goto cleanup; }

    printf( "Building target: %s\n", target->name );

    // --- 3. Directory Creation ---

#if defined( _WIN32 )
    if ( _access( "bin", 0 ) != 0 ) system( "mkdir bin" );
    if ( _access( g_build_dir, 0 ) != 0 ) { char c[BT_PATH_MAX]; snprintf(c, sizeof(c), "mkdir %s", g_build_dir); system(c); }

    char int_root[ BT_PATH_MAX ];
    snprintf( int_root, sizeof( int_root ), "%s\\%s", g_build_dir, g_int_dir );
    if ( _access( int_root, 0 ) != 0 ) { char c[BT_PATH_MAX]; snprintf(c, sizeof(c), "mkdir %s", int_root); system(c); }
    if ( _access( gen_dir, 0 ) != 0 ) { char c[BT_PATH_MAX]; snprintf(c, sizeof(c), "mkdir %s", gen_dir); system(c); }
    if ( _access( obj_dir, 0 ) != 0 ) { char c[BT_PATH_MAX]; snprintf(c, sizeof(c), "mkdir %s", obj_dir); system(c); }
#else
    char cmd_mkdir[ BT_PATH_MAX ];
    snprintf( cmd_mkdir, sizeof( cmd_mkdir ), "mkdir -p bin %s/%s/%s %s/%s", g_build_dir, g_int_dir, target->name, g_build_dir, g_gen_dir );
    system( cmd_mkdir );
#endif

    // --- 4. Locked File Management ---

    char exe_path[ BT_PATH_MAX ];
    char old_path[ BT_PATH_MAX ];
    bool renamed = false;
    snprintf( exe_path, sizeof( exe_path ), "bin\\%s.exe", target->name );
    snprintf( old_path, sizeof( old_path ), "bin\\%s.exe.old", target->name );

    if ( target->type == TARGET_EXECUTABLE && _access( exe_path, 0 ) == 0 )
    {
        remove( old_path );
        if ( rename( exe_path, old_path ) == 0 ) renamed = true;
    }

    // PDB rotation is handled at link time: each link writes a uniquely-named
    // bin/<name>_<timestamp>.pdb, so the linker never has to touch a PDB that
    // an attached debugger may hold open. See cleanup_stale_pdbs() in
    // build_tool_cc.c for the garbage-collection of unlocked leftovers.

    // --- 5. Reflection ---

    if ( target->has_reflect )
    {
        printf( "[REFL] Generating reflection for %s...\n", target->reflect_name );
        char refl_cmd[ BT_PATH_MAX * 2 ];
        snprintf( refl_cmd, sizeof( refl_cmd ), "bin\\build_reflect.exe %s %s %s", target->root_dir, gen_dir, target->reflect_name );
        if ( build_run_cmd( refl_cmd ) != 0 )
        {
            if ( renamed ) rename( old_path, exe_path );
            result = false;
            goto cleanup;
        }
    }

    // --- 6. Compile & Link ---

    if ( !build_target_compile( ctx, target, obj_dir, gen_dir ) )
    {
        if ( renamed ) rename( old_path, exe_path );
        result = false;
        goto cleanup;
    }

    if ( !build_target_link( ctx, target, obj_dir ) )
    {
        if ( renamed ) rename( old_path, exe_path );
        result = false;
        goto cleanup;
    }

cleanup:
    build_unlock_target( target_lock );
    return result;
}

/*============================================================================================*/
// --- Main Entry ---

int
main( int argc, char** argv )
{
    build_context_t ctx = { 0 };
    ctx.config = CONFIG_DEBUG;

    bool  should_clean = false;
    bool  should_gen   = false;
    char* target_name  = NULL;
    int   j_threads    = 0;   // 0 → auto-detect from CPU count.

    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-clean" ) == 0 || strcmp( argv[ i ], "clean" ) == 0 ) should_clean = true;
        if ( strcmp( argv[ i ], "-gen" ) == 0 || strcmp( argv[ i ], "gen" ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        if ( strcmp( argv[ i ], "clang" ) == 0 ) ctx.is_clang = true;
        if ( strcmp( argv[ i ], "-no-deps" ) == 0 ) ctx.skip_deps = true;
        if ( strcmp( argv[ i ], "-target" ) == 0 && i + 1 < argc ) target_name = argv[ ++i ];
        if ( strcmp( argv[ i ], "-j" ) == 0 && i + 1 < argc ) j_threads = atoi( argv[ ++i ] );
        if ( strcmp( argv[ i ], "-config" ) == 0 && i + 1 < argc )
        {
            if ( _stricmp( argv[ ++i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        }
    }

    // Auto-pick worker count = logical processor count, capped sensibly.
    if ( j_threads <= 0 )
    {
        SYSTEM_INFO si;
        GetSystemInfo( &si );
        j_threads = ( int )si.dwNumberOfProcessors;
        if ( j_threads < 1 ) j_threads = 1;
        if ( j_threads > 16 ) j_threads = 16;
    }

    if ( should_clean ) { build_clean(); return 0; }
    if ( should_gen ) { build_gen_projects(); return 0; }

    printf( "--- ORB Build Starting ---\n\n" );
    build_setup_vc_env();

    printf( "Config: %s\n", ctx.config == CONFIG_DEBUG ? "Debug" : "Release" );
    printf( "Compiler: %s\n\n", ctx.is_clang ? "Clang" : "MSVC" );

    // Dispatch:
    //  -no-deps           → serial in-process build of exactly one target.
    //                       VS uses this so MSBuild's scheduler stays the sole
    //                       authority on solution-level parallelism.
    //  -target X          → parallel scheduler over X's dependency closure.
    //  no -target         → parallel scheduler over every registered target.
    target_info_t* target = NULL;
    if ( target_name )
    {
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( _stricmp( g_targets[ i ].name, target_name ) == 0 ) { target = &g_targets[ i ]; break; }
        }
        if ( !target ) { printf( "Error: Unknown target '%s'\n", target_name ); return 1; }
    }

    if ( ctx.skip_deps )
    {
        if ( !target ) { printf( "Error: -no-deps requires -target.\n" ); return 1; }
        if ( !build_target( &ctx, target ) ) { printf( "\nFAILED!\n" ); return 1; }
    }
    else
    {
        if ( !build_run_parallel( &ctx, target, j_threads ) )
        {
            printf( "\nFAILED!\n" );
            return 1;
        }
    }

    printf( "\nSUCCESS!\n" );
    return 0;
}

// clang-format on
/*============================================================================================*/
