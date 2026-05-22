/*==============================================================================================

    build_tool.c -- The "Boss" build orchestrator.

    This tool is the heart of ORB's custom build system. It replaces Makefiles /
    CMake / MSBuild with a small C program that directly invokes the compiler
    (cl.exe), linker (link.exe), and archiver (lib.exe), and that also generates
    .sln/.vcxproj files so Visual Studio can attach a debugger and provide
    IntelliSense without taking over the build itself.

    Unity build layout:
        This file #includes every other .c in the directory in dependency order.
        The whole tool compiles in one cl.exe invocation. That keeps the
        bootstrap script (bootstrap_build_tool.bat) to a single command line
        and lets later files use static helpers declared in earlier files
        without having to expose them in the header.

    Module roles (in the include order below):
        build_tool_utils.c   -- cmd_buf, mtime, per-target named-mutex lock.
        build_tool_vcvars.c  -- VS env discovery + CreateProcess child spawn.
        build_tool_cc.c      -- cl.exe / link.exe / lib.exe command emission.
        build_tool_targets.c -- The actual g_targets[] / g_solutions[] data.
        build_tool_gen.c     -- .sln / .vcxproj / .filters generation.
        build_tool_sched.c   -- Topological worker-pool parallel scheduler.
        build_tool_exec.c    -- Build execution: artifact clean + build_target driver.

==============================================================================================*/
// clang-format off

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "build_tool.h"

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <io.h>
#include <process.h>

#if defined( _WIN32 )
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN
    #include <windows.h>
#endif

// =============================================================================

// --- Project Constants ---
//
// All build outputs land under <g_build_dir>/. The .sln/.vcxproj files also
// live here (see build_tool_gen.c) so VS treats the directory as the
// project root for its IntelliSense and intermediate caches.

static const char* g_build_dir  = "build_new";      // Root for intermediate/generated files.
static const char* g_int_dir    = "obj";            // Sub-folder for .obj files (per-target).
static const char* g_gen_dir    = "generated";      // Sub-folder for reflection-generated .c/.h.

// --- Output Format ---
//
// ORB_BANNER: indent for top-level orb lines  e.g.  "      [ orb build: X ]"
// ORB_INDENT: indent for sub-lines            e.g.  "          [build] foo ..."
// Defined here (before unity includes) so build_tool_sched.c can use them.
#define ORB_BANNER  "      "
#define ORB_INDENT  "          "

// --- Output flags ---
//
// Process-global verbosity mask. Set once in main() from CLI args; all
// unity-included modules read it via the extern declared in build_tool.h.
out_flags_t g_out_flags = ORB_OUT_DEFAULT;

// --- Unity Includes ---
//
// Order matters: each file may reference statics defined in earlier files.
// utils -> vcvars -> cc -> targets (pure data) -> gen -> sched.

#include "build_tool_utils.c"
#include "build_tool_vcvars.c"
#include "build_tool_cc.c"
#include "build_tool_targets.c"
#include "build_tool_gen.c"
#include "build_tool_sched.c"
#include "build_tool_exec.c"

/*============================================================================================*/
// --- Main Entry ---
//
// Recognized arguments:
//   -clean / clean          Wipe build outputs and exit.
//   -gen   / gen            Regenerate .sln/.vcxproj and exit.
//   -target <name>          Restrict the build to one target's closure.
//   -compile-only           Compile all unity units for the target, no link step.
//                           Used by VS Ctrl+F7 via NMakeCompileFileCommandLine.
//                           Requires -target.
//   -file <path>            Compile a single file with the target's full flag set.
//                           No link step. CLI tool for targeted error checking.
//                           Requires -target.
//   -no-deps                Build only the target itself, no dep recursion
//                           (set by VS .vcxproj files; do not use on the CLI
//                           unless you know exactly what you're doing).
//   -monolithic             Build DLL modules as static libs; defines BUILD_STATIC globally.
//   -config <Debug|Release> Pick build config (default Debug).
//   release                 Shortcut for -config Release.
//   clang                   Use clang-cl instead of cl.exe.
//   -j N                    Worker thread count; 0/unset = auto-detect.

int
main( int argc, char** argv )
{
    // Uncomment to log every invocation to build_tool_log.txt (useful for debugging VS integration).
    // {
    //     FILE* log = fopen( "build_tool_log.txt", "a" );
    //     if ( log )
    //     {
    //         fprintf( log, "argc=%d", argc );
    //         for ( int i = 0; i < argc; ++i ) fprintf( log, "  [%d]=%s", i, argv[ i ] );
    //         fprintf( log, "\n" );
    //         fclose( log );
    //     }
    // }

    build_context_t ctx = { 0 };
    ctx.config   = BT_CONFIG_DEBUG;
    ctx.compiler = BT_COMPILER_MSVC; 

    bool  should_clean   = false; 
    bool  should_gen     = false;
    bool  compile_only   = false;  // -compile-only: compile all units, no link (VS Ctrl+F7).
    char* target_name    = NULL;
    char* file_path      = NULL;   // -file <path>: compile one file (CLI use).
    int   j_threads      = 0;     // 0 → auto-detect from CPU count.

    // --- Arg parsing ---
    // Order-independent; the loop just sets flags. Unknown args are silently
    // ignored (intentional — keeps backward compat with VS External Tools
    // call sites that might pass legacy positional args).
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-clean" ) == 0 || strcmp( argv[ i ], "clean" ) == 0 ) should_clean = true;
        if ( strcmp( argv[ i ], "-gen" ) == 0 || strcmp( argv[ i ], "gen" ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "release" ) == 0 ) ctx.config = BT_CONFIG_RELEASE;
        if ( strcmp( argv[ i ], "clang" ) == 0 ) ctx.compiler = BT_COMPILER_CLANG;
        if ( strcmp( argv[ i ], "-no-deps" ) == 0 ) ctx.skip_deps = true;
        if ( strcmp( argv[ i ], "-monolithic" ) == 0 || strcmp( argv[ i ], "monolithic" ) == 0 ) ctx.is_monolithic = true;
        if ( strcmp( argv[ i ], "-target"       ) == 0 && i + 1 < argc ) target_name  = argv[ ++i ];
        if ( strcmp( argv[ i ], "-file"         ) == 0 && i + 1 < argc ) file_path    = argv[ ++i ];
        if ( strcmp( argv[ i ], "-compile-only" ) == 0 ) compile_only = true;
        if ( strcmp( argv[ i ], "-j" ) == 0 && i + 1 < argc ) j_threads = atoi( argv[ ++i ] );
        if ( strcmp( argv[ i ], "-config" ) == 0 && i + 1 < argc )
        {
            if ( _stricmp( argv[ ++i ], "release" ) == 0 ) ctx.config = BT_CONFIG_RELEASE;
        }
        // Output verbosity. -q / -v are preset shorthands; --out takes a hex
        // mask so individual sections can be toggled without recompiling.
        if ( strcmp( argv[ i ], "-q" ) == 0 ) g_out_flags = ORB_OUT_QUIET;
        if ( strcmp( argv[ i ], "-v" ) == 0 ) g_out_flags = ORB_OUT_VERBOSE;
        if ( strcmp( argv[ i ], "--out" ) == 0 && i + 1 < argc )
            g_out_flags = (out_flags_t)strtoul( argv[ ++i ], NULL, 16 );
    }

    // --- Worker count ---
    // Auto-pick = logical processor count, clamped to [1, 16]. The upper
    // cap matches diminishing returns once we exceed the dep graph's
    // critical-path width; more threads past that just trade memory for
    // wall time wins that don't materialize.
    if ( j_threads <= 0 )
    {
        SYSTEM_INFO si;
        GetSystemInfo( &si );
        j_threads = ( int )si.dwNumberOfProcessors;
        if ( j_threads < 1 ) j_threads = 1;
        if ( j_threads > 16 ) j_threads = 16;
    }

    // --- Standalone subcommands ---
    // clean and gen are bookkeeping passes — no need to set up the toolchain
    // env. Returning early skips the vcvars import below.
    if ( should_clean )
    {
        target_info_t* clean_target = NULL;
        if ( target_name )
        {
            clean_target = find_target_icase( target_name );
            if ( !clean_target ) { printf( ORB_INDENT "[orb error] unknown target '%s'\n", target_name ); return 1; }
        }
        build_clean( clean_target );
        return 0;
    }
    if ( should_gen ) { build_gen_projects(); return 0; }


    // --- Startup Banner ---
    // Single-file mode shows the filename; full-build mode shows the target name.

    char target_upper[ 64 ] = "ALL";
    if ( target_name )
    {
        int k = 0;
        for ( ; target_name[ k ] && k < (int)sizeof( target_upper ) - 1; ++k )
            target_upper[ k ] = (char)toupper( (unsigned char)target_name[ k ] );
        target_upper[ k ] = '\0';
    }

    if ( compile_only )
    {
        printf( ORB_BANNER "[orb compile] %s %s\n", target_upper,
                ctx.config == BT_CONFIG_DEBUG ? "Debug" : "Release" );
    }
    else if ( file_path )
    {
        const char* basename = file_path;
        for ( const char* p = file_path; *p; ++p )
            if ( *p == '\\' || *p == '/' ) basename = p + 1;
        printf( ORB_BANNER "[orb file] %s/%s %s\n", target_upper,
                basename, ctx.config == BT_CONFIG_DEBUG ? "Debug" : "Release" );
    }
    else
    {
        printf( ORB_BANNER "[orb build] %s ", target_upper );
        printf( "%s%s%s |", ctx.config == BT_CONFIG_DEBUG ? "Debug" : "Release",
                            ctx.is_monolithic ? " | Monolithic" : " | Dynamic",
                            ctx.compiler == BT_COMPILER_CLANG ? " | Clang" : "" );
        for ( int i = 1; i < argc; ++i ) printf( " %s", argv[ i ] );
        printf( "\n" );
    }


    // Make cl.exe / link.exe / lib.exe callable. Idempotent fast-path when
    // we're already inside a Developer Command Prompt (or VS-launched shell).
    build_setup_vc_env();

    // Dispatch:
    //  -file <path>       → compile one file with the target's full flag set, no link.
    //                       CLI tool for targeted error checking; not used by VS.
    //  -no-deps           → serial in-process build of exactly one target.
    //                       VS uses this so MSBuild's scheduler stays the sole
    //                       authority on solution-level parallelism.
    //  -target X          → parallel scheduler over X's dependency closure.
    //  no -target         → parallel scheduler over every registered target.
    target_info_t* target = NULL;
    if ( target_name )
    {
        target = find_target_icase( target_name );
        if ( !target ) { printf( ORB_INDENT "[orb error] unknown target '%s'\n", target_name ); return 1; }
    }

    if ( compile_only )
    {
        // VS Ctrl+F7 path: compile all unity units for the target, no link step.
        // VS does not inject the selected file path via any env/property mechanism,
        // so we compile the full unity TU instead — correct for unity-build targets.
        if ( !target ) { printf( ORB_INDENT "[orb error] -compile-only requires -target\n" ); return 1; }
        if ( !build_target_compile_only( &ctx, target ) )
        {
            printf( ORB_BANNER "[ FAILED: %s ]\n", target_upper );
            return 1;
        }
        printf( "\n" );
        return 0;
    }

    if ( file_path )
    {
        // Single-file compile: build_target_compile_single() with the target's full
        // flag/define/include set, but only the one file VS handed us.
        if ( !target ) { printf( ORB_INDENT "[orb error] -file requires -target\n" ); return 1; }

        char obj_dir[ BT_PATH_MAX ];
        snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
        char gen_dir[ BT_PATH_MAX ];
        snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );

        // Ensure the obj dir exists (normally created by a prior full build, but
        // be defensive so the first-ever single-file compile doesn't silently fail).
        if ( _access( "bin",       0 ) != 0 ) system( "mkdir bin" );
        if ( _access( g_build_dir, 0 ) != 0 ) { char c[ BT_PATH_MAX ]; snprintf( c, sizeof(c), "mkdir %s",          g_build_dir ); system( c ); }
        if ( _access( obj_dir,     0 ) != 0 ) { char c[ BT_PATH_MAX ]; snprintf( c, sizeof(c), "mkdir %s", obj_dir );              system( c ); }

        if ( !build_target_compile_single( &ctx, target, obj_dir, gen_dir, file_path ) )
        {
            printf( ORB_BANNER "[ FAILED: %s ]\n", target_upper );
            return 1;
        }
        printf( "\n" );
        return 0;
    }

    if ( ctx.skip_deps )
    {
        if ( !target ) { printf( ORB_INDENT "[orb error] -no-deps requires -target\n" ); return 1; }
        bool was_skipped = false;
        if ( !build_target( &ctx, target, &was_skipped ) )
        {
            printf( ORB_BANNER "[ FAILED: %s ]\n", target_upper );
            return 1;
        }
        if ( g_out_flags & ORB_OUT_TARGET_RESULT )
            printf( ORB_INDENT "[orb %s] %s\n", was_skipped ? "skipped" : "compiled", target->name );
    }
    else
    {
        if ( !build_run_parallel( &ctx, target, j_threads ) )
        {
            printf( ORB_BANNER "[ FAILED: %s ]\n", target_upper );
            return 1;
        }
    }

    printf( "\n" );
    return 0;
}

// clang-format on
/*============================================================================================*/
