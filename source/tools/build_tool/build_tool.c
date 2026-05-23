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
#else 
    #error "build_tool only supports Windows (MSVC)"
#endif

/*==============================================================================================
    --- Project Constants ---

    All build outputs land under <g_build_dir>/. The .sln/.vcxproj files also live
    here (see build_tool_gen.c) so VS treats the directory as the project root for 
    its IntelliSense and intermediate caches.

    REMEMBER: Update bootstrap_build_tool.bat if you change these!

==============================================================================================*/

static const char* g_build_dir  = "build";          // Root for intermediate/generated files.
static const char* g_int_dir    = "obj";            // Sub-folder for .obj files (per-target).
static const char* g_gen_dir    = "generated";      // Sub-folder for reflection-generated .c/.h.

/*==============================================================================================
    --- Output Format ---

    ORB_BANNER: indent for top-level orb lines  e.g.  "      [ orb build: X ]"
    ORB_INDENT: indent for sub-lines            e.g.  "          [build] foo ..."    
==============================================================================================*/

#define ORB_BANNER  "      "
#define ORB_INDENT  "          "

/*==============================================================================================
    --- Output Flags ---

    Process-global verbosity mask. Set once in main() from CLI args; all
    unity-included modules read it via the extern declared in build_tool.h.
==============================================================================================*/

out_flags_t g_out_flags = ORB_OUT_DEFAULT;

/*==============================================================================================
    --- Unity Include ---

    Order matters: each file may reference statics defined in earlier files.
    utils -> vcvars -> cc -> targets (pure data) -> gen -> sched.
==============================================================================================*/

#include "build_tool_utils.c"
#include "build_tool_vcvars.c"
#include "build_tool_cmd.c"
#include "build_tool_cc.c"
#include "build_tool_targets.c"
#include "build_tool_gen.c"
#include "build_tool_sched.c"
#include "build_tool_exec.c"

/*==============================================================================================
    --- Main Entry ---
                                Recognized arguments: (all case insensitive)

      -clean                    Wipe build outputs and exit.
      -gen                      Regenerate .sln/.vcxproj and exit.
      -target <name>            Restrict the build to one target's closure.
      -compile-only             Compile all unity units for the target, no link step.
                                Used by VS Ctrl+F7 via NMakeCompileFileCommandLine.
                                Requires -target.
      -file <path>              Compile a single file with the target's full flag set.
                                No link step. CLI tool for targeted error checking.
                                Requires -target.
      -force                    Skip the up-to-date check and always compile + link.
                                Useful for testing recompilation without touching timestamps.
                                Does not wipe obj/ — use -clean for a full scrub.
      -no-deps                  Build only the target itself, no dep recursion
                                (set by VS .vcxproj files; manages deps itself.
                                (do not use on the CLI unless you know what you're doing).
      -monolithic               Build DLL modules as static libs; defines BUILD_STATIC globally.
      -config <Debug|Release>   Pick build config (default Debug)
      -release                  Shortcut for -config Release.
      -clang                    Use clang-cl instead of cl.exe.
      -j N                      Worker thread count; 0/unset = auto-detect.

==============================================================================================*/

int
main( int argc, char** argv )
{
    // -- - Debug Arg Print ---
    // debug print args to file so we can read even if not debugging with a console attached.
    // This is invaluable for verifying that the bootstrap script is passing args correctly,
    // and for diagnosing arg parsing bugs in general.

    if ( 0 )
    {
        FILE* log = fopen( "build_tool_log.txt", "a" );
        if ( log )
        {
            fprintf( log, "argc=%d", argc );
            for ( int i = 0; i < argc; ++i ) fprintf( log, "  [%d]=%s", i, argv[ i ] );
            fprintf( log, "\n" );
            fclose( log );
    
            // debug output as another sanity check.
            printf( "build_tool.exe: [%d] ", argc );
            for ( int i = 1; i < argc; ++i ) { printf( "%s ", argv[ i ] ); }
            printf( "\n\n" );
        }
    }

    // -- Build Tool Defaults -- 

    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;        // default to debug; override with -config or -release.
    ctx.compiler = COMPILE_MSVC;        // default to cl.exe; override with -clang.

    // --- Arg parsing ---
    
    bool  should_clean   = false;       // -clean: delete all build outputs and exit, no build.
    bool  should_gen     = false;       // -gen: generate .sln/.vcxproj files and exit, no build.
    bool  compile_only   = false;       // -compile-only: compile all units, no link (VS Ctrl+F7).
    char* target_name    = NULL;        // -target <name>: restrict the build to one target (VS and CLI).
    char* file_path      = NULL;        // -file <path>: compile one file (CLI use) no link.
    int   j_threads      = 0;           // 0 → auto-detect from CPU count.

    // Order-independent; the loop just sets flags. Unknown args are silently ignored.
    // NOTE: Hyphens are now mandatory for all flags. Dash-less legacy positional args are ignored.
    for ( int i = 1; i < argc; ++i )
    {
        // Simple flags that just set a bool or enum field. Case-insensitive for user-friendliness.
        if ( _stricmp( argv[ i ], "-clean"        ) == 0 ) should_clean = true;
        if ( _stricmp( argv[ i ], "-gen"          ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "-monolithic"   ) == 0 ) ctx.is_monolithic = true;
        if ( _stricmp( argv[ i ], "-release"      ) == 0 ) ctx.config = CONFIG_RELEASE;
        if ( _stricmp( argv[ i ], "-clang"        ) == 0 ) ctx.compiler = COMPILE_CLANG;
        if ( _stricmp( argv[ i ], "-compile-only" ) == 0 ) compile_only = true;
        if ( _stricmp( argv[ i ], "-force"        ) == 0 ) ctx.force_rebuild = true;
        if ( _stricmp( argv[ i ], "-no-deps"      ) == 0 ) ctx.skip_deps = true;
        if ( _stricmp( argv[ i ], "-target"       ) == 0 && i + 1 < argc ) target_name = argv[ ++i ];
        if ( _stricmp( argv[ i ], "-file"         ) == 0 && i + 1 < argc ) file_path = argv[ ++i ];        
        if ( _stricmp( argv[ i ], "-j"            ) == 0 && i + 1 < argc ) j_threads = atoi( argv[ ++i ] );
        if ( _stricmp( argv[ i ], "-config"       ) == 0 && i + 1 < argc )
        {
            if ( _stricmp( argv[ ++i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        }
        if ( _stricmp( argv[ i ], "-q"            ) == 0 ) g_out_flags = ORB_OUT_QUIET;
        if ( _stricmp( argv[ i ], "-v"            ) == 0 ) g_out_flags = ORB_OUT_VERBOSE;
        if ( _stricmp( argv[ i ], "--out"         ) == 0 && i + 1 < argc )
        {
            // --out takes a hex mask for fine-control (see build_tool.h). 
            g_out_flags = (out_flags_t)strtoul( argv[ ++i ], NULL, 16 );
        }
    }

    // --- Worker count ---
    // Auto-pick = logical processor count, clamped to [1, 16]. The upper cap matches
    // diminishing returns once we exceed the dep graph's critical-path width; more
    // threads past that just trade memory for wall time wins that don't materialize.
    if ( j_threads <= 0 )
    {
        SYSTEM_INFO si; GetSystemInfo( &si );
             j_threads = ( int )si.dwNumberOfProcessors;
        if ( j_threads < 1 )  j_threads = 1;
        if ( j_threads > 16 ) j_threads = 16;
    }

    // --- Arg Validation ---
    // -file and -compile-only are mutually exclusive: -file compiles one specific file (CLI only),
    // -compile-only compiles the whole target with no link step (VS Ctrl+F7). They can never combine.
    if ( file_path && compile_only )
    {
        printf( ORB_INDENT "[orb error] -file and -compile-only are mutually exclusive\n" );
        return 1;
    }

    // --- Command : CLEAN ---
    if ( should_clean )
    {
        target_info_t* clean_target = NULL;
        if ( target_name ) {
            clean_target = find_target_icase( target_name );
            if ( clean_target == NULL ) { 
                printf( ORB_INDENT "[orb error] unknown target '%s'\n", target_name ); 
                return 1; 
            }
        }
        build_clean( clean_target );
        return 0;
    }

    // --- Command : GENERATE ---
    if ( should_gen ) 
    { 
        build_gen_projects(); 
        return 0; 
    }

    // Note: commands return early to skip the vcvars import below.


    // --- Startup Banner ---
    // All modes share the format:  [orb <mode>] SUBJECT  [ <special> | modular | debug | msvc ]
    // Each branch sets label/subject/special; props and the final printf are assembled once below.
    // ORB_OUT_ARGS appends a second line echoing the raw argv (off by default).

    char target_upper[ 64 ] = "ALL";
    if ( target_name ) get_target_upper( target_name, target_upper );

    // base_name is filename acquired via -file with path stripped.
    const char* base_name = file_path ? file_path : NULL;
    if ( base_name ) {
        for ( const char* p = file_path; *p; ++p )
            if ( *p == '\\' || *p == '/' ) base_name = p + 1;
    }

    if ( 1 )
    {
        char subject[ BT_PATH_MAX ]; 
        snprintf( subject, sizeof( subject ), "%s", target_upper );

        const char* config_str   = ctx.config == CONFIG_DEBUG ? "debug" : "release";
        const char* mode_str     = ctx.is_monolithic ? "monolithic" : "modular";
        const char* compiler_str = ctx.compiler == COMPILE_CLANG ? "clang" : "msvc";

        const char* label        = NULL; // [orb ...]
        const char* special      = NULL; // first props slot, or NULL

        if ( compile_only )  
        {
            label   = "[orb compile-only]";
            special = "no-link";
        }
        else if ( file_path )
        {
            label   = "[orb single-file]";
            special = "file";

            // add the filename next to the target label.
            snprintf( subject, sizeof( subject ), "%s %s", target_upper, base_name );
        }
        else
        {
            label   = "[orb target]";
            special = ctx.skip_deps ? "no-deps" : NULL;
        }

        // Assemble the property bracket (special slot is mode-specific and optional).
        char props[ 128 ];
        if ( special )
            snprintf( props, sizeof( props ), "[ %s | %s | %s | %s ]", special, mode_str, config_str, compiler_str );
        else
            snprintf( props, sizeof( props ), "[ %s | %s | %s ]", mode_str, config_str, compiler_str );

        printf( ORB_BANNER "----------------------------------------------------------------\n" );
        printf( ORB_BANNER "%s %s %s\n", label, subject, props );

        // ORB_OUT_ARGS: echo the raw command line on a second line (off by default).
        // Ex: build_tool.exe -target sb_engine_reflect -config Release
        //         args:  -target sb_engine_reflect  -config Release
        if ( g_out_flags & ORB_OUT_ARGS )
        {
            printf( ORB_INDENT "[orb args]" );
            for ( int i = 1; i < argc; ++i ) printf( " %s", argv[ i ] );
            printf( "\n" );
        }
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
            printf( ORB_BANNER "[ %s: FAILED ]\n", target_upper );
            return 1;
        }
        if ( g_out_flags & ORB_OUT_TARGET_RESULT )
            printf( ORB_INDENT "[orb completed] %s\n", target->name );
        printf( "\n" );
        return 0;
    }

    if ( file_path )
    {
        // Single-file compile: build_target_compile_single() with the target's full
        // flag/define/include set, but only the one file VS handed us.
        if ( !target ) { printf( ORB_INDENT "[orb error] -file requires -target\n" ); return 1; }

        // If the path is not absolute, resolve it relative to the target's root_dir
        // so callers can pass bare filenames or subdir-relative paths like sub/file.c.
        char resolved_file[ BT_PATH_MAX ];
        bool is_abs = ( file_path[ 0 ] == '\\' ) || ( file_path[ 1 ] == ':' );
        if ( !is_abs && target->root_dir )
        {
            char combined[ BT_PATH_MAX ];
            snprintf( combined, sizeof( combined ), "%s\\%s", target->root_dir, file_path );
            if ( !_fullpath( resolved_file, combined, sizeof( resolved_file ) ) )
                snprintf( resolved_file, sizeof( resolved_file ), "%s", combined );
            file_path = resolved_file;
        }

        char obj_dir[ BT_PATH_MAX ];
        snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
        char gen_dir[ BT_PATH_MAX ];
        snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );

        // Ensure the obj dir exists (normally created by a prior full build, but
        // be defensive so the first-ever single-file compile doesn't silently fail).
        ensure_dir( "bin" );
        ensure_dir( g_build_dir );
        ensure_dir( obj_dir );

        if ( !build_target_compile_single( &ctx, target, obj_dir, gen_dir, file_path ) )
        {
            printf( ORB_BANNER "[ %s: FAILED ]\n", target_upper );
            return 1;
        }
        if ( g_out_flags & ORB_OUT_TARGET_RESULT )
            printf( ORB_INDENT "[orb completed] %s\n", base_name ); 
        printf( "\n" );
        return 0;
    }

    if ( ctx.skip_deps )
    {
        if ( !target ) { printf( ORB_INDENT "[orb error] -no-deps requires -target\n" ); return 1; }
        bool was_skipped = false;
        if ( !build_target( &ctx, target, &was_skipped ) )
        {
            printf( ORB_BANNER "\n[ %s: FAILED ]\n", target_upper );
            return 1;
        }

        bool show_output  = ( g_out_flags & ORB_OUT_TARGET_RESULT );
        bool show_skipped = was_skipped && ( g_out_flags & ORB_OUT_SUMMARY_COMPILE );
        if ( show_output || show_skipped )
            printf( ORB_INDENT "[orb %s] %s\n", was_skipped ? "skipped" : "completed", target->name );
    }
    else
    {
        // --- Build All Targets In Parallel ---
        if ( !build_run_parallel( &ctx, target, j_threads ) )
        {
            printf( ORB_BANNER "\n[ %s: FAILED ]\n", target_upper );
            return 1;
        }
    }

    printf( "\n" );
    return 0;
}

// clang-format on
/*============================================================================================*/
