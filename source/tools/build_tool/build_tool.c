/*==============================================================================================

    build_tool.c -- ORB build orchestrator.

    Replaces Makefiles / CMake / MSBuild with a small C program that directly
    invokes cl.exe, link.exe, and lib.exe. Also generates .sln/.vcxproj files so
    Visual Studio can attach a debugger and provide IntelliSense without owning
    the build itself.

    Unity build layout:
        This file #includes every other .c in the directory in dependency order.
        The whole tool compiles in one cl.exe invocation -- keeps the bootstrap
        script to a single command line and lets later files call static helpers
        from earlier files without header exposure.

    Module roles (in include order):
        build_tool_utils.c   -- cmd_buf, mtime, per-target named-mutex lock.
        build_tool_vcvars.c  -- VS env discovery + one-time vcvars import.
        build_tool_cc.c      -- cl.exe / link.exe / lib.exe command emission.
        build_tool_targets.c -- g_targets[] / g_solutions[] data tables.
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

    All build outputs land under <g_build_dir>/. The .sln/.vcxproj files live here
    too so VS treats it as the project root for IntelliSense and intermediate caches.

    NOTE: Update bootstrap_build_tool.bat if you change these paths.

==============================================================================================*/

static const char* g_build_dir  = "build";      // Root for intermediate/generated files.
static const char* g_int_dir    = "obj";        // Sub-folder for .obj files (per-target).
static const char* g_gen_dir    = "generated";  // Sub-folder for reflection-generated .c/.h.

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

out_flags_t g_out_flags  = ORB_OUT_DEFAULT;
bool        g_use_rsp    = false;
bool        g_dep_track  = true;

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
#include "build_tool_test.c"

/*==============================================================================================
    --- Startup Banner ---

    Prints a standardized banner at the start of every build with the active configuration
==============================================================================================*/

static void
print_startup_banner( const build_context_t* ctx )
{
    // All modes share the format:  [orb <mode>] SUBJECT  [ <special> | modular | debug | msvc ]
    // Each branch sets label/subject/special; the final printf is assembled once at the bottom.
    char target_upper[ 64 ] = "ALL";
    if ( ctx->target_name ) 
        get_target_upper( ctx->target_name, target_upper, sizeof( target_upper ) );

    // base_name is the filename from -file with path stripped.
    const char* file_name = ctx->file_path;
    if ( file_name ) {
        for ( const char* p = ctx->file_path; *p; ++p )
            if ( *p == '\\' || *p == '/' ) file_name = p + 1;
    }

    const char* config_str   = ctx->config == CONFIG_DEBUG ? "debug" : "release";
    const char* mode_str     = ctx->is_monolithic ? "monolithic" : "modular";
    const char* compiler_str = ctx->compiler == COMPILE_CLANG ? "clang" : "msvc";
    const char* label        = NULL;
    const char* special      = NULL;

    char subject[ BT_PATH_MAX ];
    snprintf( subject, sizeof( subject ), "%s", target_upper );

    if ( ctx->compile_only )
    {
        label   = "[orb compile-only]";
        special = "no-link";
    }
    else if ( ctx->file_path )
    {
        label   = "[orb single-file]";
        special = "file";

        snprintf( subject, sizeof( subject ), "%s %s", target_upper, file_name );
    }
    else
    {
        label   = "[orb build]";
        special = ctx->skip_deps ? "no-deps" : NULL;
    }

    char props[ 128 ];
    if ( special ) { snprintf( props, sizeof( props ), 
                            "[ %s | %s | %s | %s ]", special, mode_str, config_str, compiler_str ); }
    else           { snprintf( props, sizeof( props ), 
                            "[ %s | %s | %s ]", mode_str, config_str, compiler_str ); }

    printf( ORB_BANNER "----------------------------------------------------------------\n" );
    printf( ORB_BANNER "%s %s %s\n", label, subject, props );
}

/*==============================================================================================
    --- validate_targets ---

    Sanity-check the target and solution tables before any build work begins.
    Catches slot-array overflows and unresolved solution-to-target name references.

==============================================================================================*/

static bool
validate_targets( void )
{ 
    bool ok = true;

    // Check each target's slot arrays don't overflow.
    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];

        if ( t->units    [ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->deps     [ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->tool_deps[ TARGET_MAX_SLOTS - 1 ] != NULL )
        {
            printf( ORB_INDENT "[orb error] target '%s' has too many units or dependencies "
                               "(raise TARGET_MAX_SLOTS)\n", t->name );
            ok = false;
        }
    }

    // Check every solution's target list resolves to a known target.
    for ( int i = 0; i < g_solution_count; ++i )
    {
        const solution_info_t* sln = &g_solutions[ i ];
        for ( const char** tn = sln->target_names; *tn; ++tn )
        {
            bool found = false;
            for ( int j = 0; j < g_target_count; ++j )
            {
                if ( strcmp( g_targets[ j ].name, *tn ) == 0 ) { found = true; break; }
            }
            if ( !found )
            {
                printf( ORB_INDENT "[orb error] solution '%s' references unknown target '%s'\n",
                        sln->name, *tn );
                ok = false;
            }
        }
    }

    return ok;
}

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
                                Does not wipe obj/ -- use -clean for a full scrub.
      -no-deps                  Build only the target itself, no dep recursion
                                (set by VS .vcxproj files; manages deps itself.
                                (do not use on the CLI unless you know what you're doing).
      -monolithic, -mono        Build DLL modules as static libs; defines BUILD_STATIC globally.
      -no-rsp                   Pass full command lines directly; skip response file (.rsp) creation.
                                Safe on small projects. Default: rsp enabled (required at ~7000 chars).
      -no-dep-track             Skip /showIncludes parsing and _deps.txt read/write.
                                Up-to-date check falls back to artifact mtime vs source files only.
                                Header changes will not trigger rebuilds.
      -config <Debug|Release>   Pick build config (default Debug)
      -release                  Shortcut for -config Release.
      -clang                    Use clang-cl instead of cl.exe.
      -j N                      Worker thread count; 0/unset = auto-detect.

==============================================================================================*/

int
main( int argc, char** argv )
{
    // Inject debug args from build_tool_debug.args (see build_tool_test.c).
    // No-op in Release builds and when compiled with /DBUILD_TOOL_NO_DEBUG_INJECT.
    build_tool_debug_inject( &argc, &argv );

    // --- Debug Arg Log ---

    if ( 0 )
    {
        FILE* log = fopen( "build_tool_log.txt", "a" );
        if ( log ) 
        {
            fprintf( log, "build_tool.exe: p%d]", argc );
            for ( int i = 0; i < argc; ++i ) fprintf( log, " [%d]=%s", i, argv[ i ] );
            fprintf( log, "\n" );
            fclose( log );
        }
    }

    // --- Build Tool Defaults ---

    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;        // override with -config or -release.
    ctx.compiler = COMPILE_MSVC;        // override with -clang.

    // --- Arg parsing ---
    
    bool  should_clean   = false;       // -clean: delete all build outputs and exit, no build.
    bool  should_gen     = false;       // -gen: generate .sln/.vcxproj files and exit, no build.
    int   j_threads      = 0;           // 0 -> auto-detect from CPU count.

    // Order-independent flag scan. All flags require a leading hyphen; unknown args are ignored.

    for ( int i = 1; i < argc; ++i )
    {
        if ( _stricmp( argv[ i ], "-clean"        ) == 0 ) should_clean = true;
        if ( _stricmp( argv[ i ], "-gen"          ) == 0 ) should_gen = true;
        if ( _stricmp( argv[ i ], "-monolithic" ) == 0 ) ctx.is_monolithic = true;
        if ( _stricmp( argv[ i ], "-mono"       ) == 0 ) ctx.is_monolithic = true;
        if ( _stricmp( argv[ i ], "-no-rsp"       ) == 0 ) g_use_rsp   = false;
        if ( _stricmp( argv[ i ], "-no-dep-track" ) == 0 ) g_dep_track = false;
        if ( _stricmp( argv[ i ], "-release"      ) == 0 ) ctx.config = CONFIG_RELEASE;
        if ( _stricmp( argv[ i ], "-clang"        ) == 0 ) ctx.compiler = COMPILE_CLANG;
        if ( _stricmp( argv[ i ], "-compile-only" ) == 0 ) ctx.compile_only = true;
        if ( _stricmp( argv[ i ], "-force"        ) == 0 ) ctx.force_rebuild = true;
        if ( _stricmp( argv[ i ], "-no-deps"      ) == 0 ) ctx.skip_deps = true;
        if ( _stricmp( argv[ i ], "-target"       ) == 0 && i + 1 < argc ) ctx.target_name = argv[ ++i ];
        if ( _stricmp( argv[ i ], "-file"         ) == 0 && i + 1 < argc ) ctx.file_path = argv[ ++i ];
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
    // Default to logical processor count, clamped to [1, 16]. Beyond ~16, the
    // dep-graph critical-path width limits further wall-time gains.
    if ( j_threads <= 0 )
    {
        SYSTEM_INFO si; GetSystemInfo( &si );
             j_threads = ( int )si.dwNumberOfProcessors;
        if ( j_threads < 1 )  j_threads = 1;
        if ( j_threads > 16 ) j_threads = 16;
    }

    // --- Arg Validation ---
    // -file (single file, no link) and -compile-only (all units, no link) are mutually exclusive.
    if ( ctx.file_path && ctx.compile_only )
    {
        printf( ORB_INDENT "[orb error] -file and -compile-only are mutually exclusive\n" );
        return 1;
    }

    // --- Target Table Validation ---

    if ( !validate_targets() ) return 1;

    // --- Command : CLEAN ---

    if ( should_clean )
    {
        target_info_t* clean_target = NULL;
        if ( ctx.target_name ) {
            clean_target = find_target_icase( ctx.target_name );
            if ( clean_target == NULL ) {
                printf( ORB_INDENT "[orb error] unknown target '%s'\n", ctx.target_name );
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

    // --- Begin Startup Banner ---

    print_startup_banner( &ctx );

    // --- ORB_OUT_ARGS ---

    if ( g_out_flags & ORB_OUT_ARGS )
    {
        printf( ORB_INDENT "[orb args]" );
        for ( int i = 1; i < argc; ++i ) printf( " %s", argv[ i ] );
        printf( "\n" );
    }
    
    // --- Setup Visual Studio Environment ---    
    // Make cl.exe / link.exe / lib.exe callable. Idempotent fast-path when
    // we're already inside a Developer Command Prompt (or VS-launched shell).

    build_setup_vc_env();

    // --- Build Dispatch ---
    // 
    //   -file <path>  compile one file with the target's full flag set, no link.
    //   -compile-only compile all unity units for the target, no link (VS Ctrl+F7).
    //   -no-deps      serial single-target build; VS uses this so MSBuild stays
    //                 the sole authority on solution-level dep ordering.
    //   -target X     parallel scheduler over X's dependency closure.
    //   (none)        parallel scheduler over all registered targets.

    char target_upper[ 64 ] = "ALL";
    if ( ctx.target_name ) get_target_upper( ctx.target_name, target_upper, sizeof( target_upper ) );

    target_info_t* target = NULL;
    if ( ctx.target_name )
    {
        target = find_target_icase( ctx.target_name );
        if ( !target ) { 
            printf( ORB_INDENT "[orb error] unknown target '%s'\n", ctx.target_name ); 
            return 1; 
        }
    }

    if ( ctx.compile_only )
    {
        // VS Ctrl+F7 path: compile all unity units for the target, no link step.
        // VS does not inject the selected file path via any env/property mechanism,
        // so we compile the full unity TU instead -- correct for unity-build targets.

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

    if ( ctx.file_path )
    {
        // Single-file compile: build_target_compile_single() with the target's full
        // flag/define/include set, but only the one file VS handed us.

        if ( !target ) { printf( ORB_INDENT "[orb error] -file requires -target\n" ); return 1; }

        // If the path is not absolute, resolve it relative to the target's root_dir
        // so callers can pass bare filenames or subdir-relative paths like sub/file.c.

        const char* effective_file = ctx.file_path;
        char resolved_file[ BT_PATH_MAX ];
        bool is_abs = ( ctx.file_path[ 0 ] == '\\' ) || ( ctx.file_path[ 1 ] == ':' );
        if ( !is_abs && target->root_dir )
        {
            char combined[ BT_PATH_MAX ];
            snprintf( combined, sizeof( combined ), "%s\\%s", target->root_dir, ctx.file_path );
            if ( !_fullpath( resolved_file, combined, sizeof( resolved_file ) ) )
                snprintf( resolved_file, sizeof( resolved_file ), "%s", combined );
            effective_file = resolved_file;
        }

        // Strip path from base_name for the completed output line.

        const char* base_name = effective_file;
        for ( const char* p = effective_file; *p; ++p )
            if ( *p == '\\' || *p == '/' ) base_name = p + 1;

        char obj_dir[ BT_PATH_MAX ];
        snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
        char gen_dir[ BT_PATH_MAX ];
        snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );

        // Ensure the obj dir exists (normally created by a prior full build, but
        // be defensive so the first-ever single-file compile doesn't silently fail).

        ensure_dir( "bin" );
        ensure_dir( g_build_dir );
        ensure_dir( obj_dir );

        if ( !build_target_compile_single( &ctx, target, obj_dir, gen_dir, effective_file ) )
        {
            printf( ORB_BANNER "[ %s: FAILED ]\n", target_upper );
            return 1;
        }
        if ( g_out_flags & ORB_OUT_TARGET_RESULT )
            printf( ORB_INDENT "[orb completed] %s\n", base_name );
        printf( "\n" );
        return 0;
    }

    /* -- Build Target(s) Visual Studio (skip deps) or CLI -- */

    if ( ctx.skip_deps )
    {
        /* visual studio invokes build_tool.exe with -no-deps to manage the dep ordering itself via MSBuild, so
           if we see that flag we skip the scheduler and just build the one target. */

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
        // --- Build All Targets In Parallel (CLI) ---

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
