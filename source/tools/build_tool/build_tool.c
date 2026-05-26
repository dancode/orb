/*==============================================================================================

    build_tool.c -- ORB build orchestrator entry point.

    Replaces Makefiles / CMake / MSBuild with a small C program that directly
    invokes cl.exe, link.exe, and lib.exe. Also generates .sln/.vcxproj files so
    Visual Studio can attach a debugger and provide IntelliSense without owning
    the build itself.

    Unity build layout:
        This file #includes every other .c in execution order. The whole tool
        compiles in one cl.exe invocation -- keeps the bootstrap script to a
        single command line and lets later files call static helpers from earlier
        files without header exposure.

    Module roles (in include order):
        01_prim.c    -- cmd_buf, mtime, file locks (pure primitives, no deps)
        02_data.c    -- g_targets[] / g_solutions[] tables + lookup helpers
        03_env.c     -- VS environment discovery and vcvars import
        04_log.c     -- stateless output formatters (print_section, etc.)
        05_spawn.c   -- child process spawning, /showIncludes capture
        06_compile.c -- cl.exe command assembly and execution
        07_link.c    -- link.exe / lib.exe command assembly and execution
        08_exec.c    -- build_target() 8-phase orchestration
        09_sched.c   -- topological worker-pool parallel scheduler
        10_clean.c   -- -clean command: per-target or global artifact wipe
        11_gen.c     -- -gen command: .sln / .vcxproj / .filters generation
        12_test.c    -- debug arg injection from build_tool_debug.args

==============================================================================================*/
// clang-format off

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "build_tool.h"

#include <stdio.h>
#include <stdlib.h>

#if defined( _WIN32 )
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <pthread.h>
    #include <semaphore.h>
    #include <spawn.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <fnmatch.h>
    #include <strings.h>
    #include <time.h>
#endif

/*==============================================================================================
    --- Output Format ---

    ORB_BANNER: indent for top-level orb lines  e.g.  "      [ orb build: X ]"
    ORB_INDENT: indent for sub-lines            e.g.  "          [build] foo ..."

    Defined here -- before the unity #includes -- so every module sees them.
==============================================================================================*/

#define ORB_BANNER  "      "
#define ORB_INDENT  "          "

/*==============================================================================================
    --- Project Constants ---

    All build outputs land under <g_build_dir>/. The .sln/.vcxproj files live here
    too so VS treats it as the project root for IntelliSense and intermediate caches.
==============================================================================================*/

static const char* g_build_dir = BUILD_DIR;     // root for VS project files + intermediates.
static const char* g_int_dir   = "obj";         // sub-folder: per-target .obj files.
static const char* g_gen_dir   = "generated";   // sub-folder: reflection-generated .c/.h.

/*==============================================================================================
    --- Output Flags ---

    Process-global verbosity mask. Set once in main() from CLI args; all
    unity-included modules read it via the extern declared in build_tool.h.
==============================================================================================*/

out_flags_t g_out_flags         = ORB_OUT_DEFAULT;
bool        g_include_track     = true;
bool        g_use_rsp           = false; /* until we hit overflow this will remain off */

/*==============================================================================================
    --- Unity Include Chain ---

    Execution order == include order. Reading top-to-bottom traces the program
    from startup through each command to the terminal dispatch in main().
==============================================================================================*/

#if defined( _WIN32 )
    #include "build_tool_win.c"             // 00a platform layer (MSVC / Win32 CRT wrappers)
    #include "build_tool_win_thread.c"      // 00b platform threading (mutex / cond / TLS / threads)
    #include "build_tool_win_spawn.c"       // 00c platform process spawning
    #include "build_tool_win_toolchain.c"   // 00d compiler and linker flags (MSVC / clang-cl)
#else
    #include "build_tool_posix.c"           // 00a platform layer (POSIX / libc wrappers)
    #include "build_tool_posix_thread.c"    // 00b platform threading (pthreads / semaphores)
    #include "build_tool_posix_spawn.c"     // 00c platform process spawning
    #include "build_tool_posix_toolchain.c" // 00d compiler and linker flags (GCC / Clang)
#endif
#include "build_tool_01_prim.c"             // 01 foundation primitives
#include "build_tool_02_data.c"             // 02 target/solution pools + built-in registrations
#include "build_tool_02_registry.c"         // 02b orb.targets text-file parser
#include "build_tool_03_env.c"              // 03 vcvars setup
#include "build_tool_04_log.c"              // 04 output formatting
#include "build_tool_05_spawn.c"            // 05 child process spawning
#include "build_tool_06_compile.c"          // 06 compile command
#include "build_tool_07_link.c"             // 07 link command
#include "build_tool_08_exec.c"             // 08 build_target orchestration
#include "build_tool_09_sched.c"            // 09 parallel scheduler
#include "build_tool_10_clean.c"            // 10 -clean command
#include "build_tool_11_gen.c"              // 11 -gen command (NMake/Makefile projects)
#include "build_tool_gen_msbuild.c"         // 11b -gen_ms command (MSBuild projects)
#include "build_tool_12_test.c"             // 12 debug arg injection

/*==============================================================================================
    --- validate_targets ---

    Sanity-check the target and solution tables before any build work begins.
    Catches slot-array overflows and unresolved solution-to-target name references.
==============================================================================================*/

static bool
validate_targets( void )
{
    bool ok = true;
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
    for ( int i = 0; i < g_solution_count; ++i )
    {
        const solution_info_t* sln = &g_solutions[ i ];
        for ( const char* const* tn = sln->target_names; *tn; ++tn )
        {
            bool found = false;
            for ( int j = 0; j < g_target_count; ++j )
                if ( strcmp( g_targets[ j ].name, *tn ) == 0 ) { found = true; break; }
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
    --- print_startup_banner ---

    Prints a standardized header at the start of every build with the active
    configuration. Calls get_target_upper() from 06_compile.c (visible: 06 is
    included before main() is reached).
==============================================================================================*/

static void
print_startup_banner( const build_context_t* ctx )
{
    char target_upper[ 64 ] = "ALL";
    if ( ctx->target_name )
        get_target_upper( ctx->target_name, target_upper, sizeof( target_upper ) );

    /* truncate file path to just the file name. */

    const char* file_name = ctx->file_path;
    if ( file_name )
        for ( const char* p = ctx->file_path; *p; ++p )
            if ( *p == '\\' || *p == '/' ) file_name = p + 1;

    const char* config_str   = ctx->config   == CONFIG_DEBUG  ? "debug" : "release";
    const char* compiler_str = ctx->compiler == COMPILE_CLANG ? "clang" : "msvc";
    const char* mode_str     = ctx->is_monolithic ? "monolithic" : "modular";
    const char* label        = NULL;
    const char* special      = NULL;

    char subject[ PATH_MAX ];
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
    if ( special )
        snprintf( props, sizeof( props ), "[ %s | %s | %s | %s ]", special, mode_str, config_str, compiler_str );
    else
        snprintf( props, sizeof( props ), "[ %s | %s | %s ]", mode_str, config_str, compiler_str );

    printf( ORB_BANNER "----------------------------------------------------------------\n" );
    printf( ORB_BANNER "%s %s %s\n", label, subject, props );
}

/*==============================================================================================
    --- Main Entry ---

    Recognized arguments (all case-insensitive):

      -clean                  Wipe build outputs and exit.
      -bootstrap              Recompile build_tool.exe itself (self-hosting).
      -gen                    Regenerate NMake .sln/.vcxproj and exit.
      -gen_ms                 Regenerate MSBuild .sln/.vcxproj and exit (better IntelliSense).
      -target <name>          Restrict build to one target's closure.
      -compile-only           Compile all unity units; no link. (VS Ctrl+F7)
      -file <path>            Compile one file with the target's full flag set; no link.
      -force                  Skip the up-to-date check; always compile + link.
      -no-deps                Build only this target; no dep recursion. (VS -managed)
      -monolithic, -mono      Build DLL modules as static libs; defines BUILD_STATIC.
      -no-rsp                 Pass full command lines directly; skip .rsp creation.
      -no-include-track       Skip /showIncludes parsing; header changes won't trigger rebuild.
      -config <Debug|Release> Pick build configuration (default: Debug).
      -release                Shortcut for -config Release.
      -clang                  Use clang-cl instead of cl.exe.
      -j N                    Worker thread count; 0/unset = auto-detect.
      -q                      Quiet: suppress most output.
      -v                      Verbose: enable all output sections.
      --out <hex>             Fine-grained output mask (see out_flags_t in build_tool.h).

==============================================================================================*/

int
main( int argc, char** argv )
{
    // --- Debug arg injection --- 
    
    build_tool_debug_inject( &argc, &argv );

    // --- Context defaults ---

    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;
    ctx.compiler = COMPILE_MSVC;

    bool should_clean        = false;
    bool should_gen          = false;
    bool should_gen_msbuild  = false;
    bool should_bootstrap    = false;
    int  j_threads           = 0;   // 0 -> auto-detect from CPU count.

    // --- Arg parsing (order-independent flag scan) ---

    for ( int i = 1; i < argc; ++i )
    {
        if ( platform_stricmp( argv[ i ], "-clean"            ) == 0 ) should_clean = true;
        if ( platform_stricmp( argv[ i ], "-gen"              ) == 0 ) should_gen = true;
        if ( platform_stricmp( argv[ i ], "-gen_ms"           ) == 0 ) should_gen_msbuild = true;
        if ( platform_stricmp( argv[ i ], "-bootstrap"        ) == 0 ) should_bootstrap = true;
        if ( platform_stricmp( argv[ i ], "-monolithic"       ) == 0 ) ctx.is_monolithic = true;
        if ( platform_stricmp( argv[ i ], "-mono"             ) == 0 ) ctx.is_monolithic = true;
        if ( platform_stricmp( argv[ i ], "-no-rsp"           ) == 0 ) g_use_rsp = false;
        if ( platform_stricmp( argv[ i ], "-no-include-track" ) == 0 ) g_include_track = false;
        if ( platform_stricmp( argv[ i ], "-release"          ) == 0 ) ctx.config = CONFIG_RELEASE;
        if ( platform_stricmp( argv[ i ], "-clang"            ) == 0 ) ctx.compiler = COMPILE_CLANG;
        if ( platform_stricmp( argv[ i ], "-compile-only"     ) == 0 ) ctx.compile_only = true;
        if ( platform_stricmp( argv[ i ], "-force"            ) == 0 ) ctx.force_rebuild = true;
        if ( platform_stricmp( argv[ i ], "-no-deps"          ) == 0 ) ctx.skip_deps = true;
        if ( platform_stricmp( argv[ i ], "-q"                ) == 0 ) g_out_flags = ORB_OUT_QUIET;
        if ( platform_stricmp( argv[ i ], "-v"                ) == 0 ) g_out_flags = ORB_OUT_VERBOSE;
        if ( platform_stricmp( argv[ i ], "-target"  ) == 0 && i + 1 < argc ) ctx.target_name = argv[ ++i ];
        if ( platform_stricmp( argv[ i ], "-file"    ) == 0 && i + 1 < argc ) ctx.file_path   = argv[ ++i ];
        if ( platform_stricmp( argv[ i ], "-j"       ) == 0 && i + 1 < argc ) j_threads       = atoi( argv[ ++i ] );
        if ( platform_stricmp( argv[ i ], "-config"  ) == 0 && i + 1 < argc )
        {
            if ( platform_stricmp( argv[ ++i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        }
        if ( platform_stricmp( argv[ i ], "--out" ) == 0 && i + 1 < argc )
        {
            g_out_flags = (out_flags_t)strtoul( argv[ ++i ], NULL, 16 );
        }
    }

    // --- Worker count: clamp logical CPU count to [1, MAX_THREADS] ---

    if ( j_threads <= 0 )
         j_threads = platform_cpu_count();

    // --- Arg validation ---

    if ( ctx.file_path && ctx.compile_only )
    {
        printf( ORB_INDENT "[orb error] -file and -compile-only are mutually exclusive\n" );
        return 1;
    }

    // --- Target registry: built-ins first, then orb.targets ---

    init_builtin_targets();
    registry_load( "orb.targets" );

    if ( !validate_targets() ) return 1;

    // --- Command: BOOTSTRAP (recompile build_tool.exe itself) ---

    if ( should_bootstrap )
    {
        build_setup_vc_env();
        target_info_t* bt = NULL;
        for ( int i = 0; i < g_target_count; ++i )
            if ( g_targets[ i ].is_build_tool ) { bt = &g_targets[ i ]; break; }
        if ( !bt ) { printf( ORB_INDENT "[orb error] build_tool target not found\n" ); return 1; }
        ctx.force_rebuild = true;
        bool skipped = false;
        if ( !build_target( &ctx, bt, &skipped ) )
        {
            printf( ORB_BANNER "[ BOOTSTRAP: FAILED ]\n" );
            return 1;
        }
        printf( ORB_BANNER "[ BOOTSTRAP: OK ]\n" );
        return 0;
    }

    // --- Command: CLEAN ---

    if ( should_clean )
    {
        target_info_t* clean_target = NULL;
        if ( ctx.target_name )
        {
            clean_target = find_target_icase( ctx.target_name );
            if ( !clean_target )
            {
                printf( ORB_INDENT "[orb error] unknown target '%s'\n", ctx.target_name );
                return 1;
            }
        }
        build_clean( clean_target );
        return 0;
    }

    // --- Command: GENERATE (NMake/Makefile projects) ---

    if ( should_gen )
    {
        build_gen_projects();
        return 0;
    }

    // --- Command: GENERATE MSBUILD (StaticLibrary/DLL/Application projects) ---

    if ( should_gen_msbuild )
    {
        build_gen_projects_msbuild();
        return 0;
    }

    // --- Banner + VC env ---

    print_startup_banner( &ctx );

    if ( g_out_flags & ORB_OUT_ARGS )
    {
        printf( ORB_INDENT "[orb args]" );
        for ( int i = 1; i < argc; ++i ) printf( " %s", argv[ i ] );
        printf( "\n" );
    }

    // Make cl.exe / link.exe / lib.exe callable. Idempotent when already inside
    // a Developer Command Prompt.
    build_setup_vc_env();

    // --- Resolve target (if named) ---

    char target_upper[ 64 ] = "ALL";
    if ( ctx.target_name )
        get_target_upper( ctx.target_name, target_upper, sizeof( target_upper ) );

    target_info_t* target = NULL;
    if ( ctx.target_name )
    {
        target = find_target_icase( ctx.target_name );
        if ( !target )
        {
            printf( ORB_INDENT "[orb error] unknown target '%s'\n", ctx.target_name );
            return 1;
        }
    }

    // --- Command: COMPILE-ONLY (VS Ctrl+F7) ---
    //
    // Compiles all unity units for the target; no link step. VS does not inject the
    // selected file path, so we compile the full unity TU -- correct for unity targets.

    if ( ctx.compile_only )
    {
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

    // --- Command: SINGLE-FILE COMPILE ---
    //
    // Compile one file with the target's full flag set; no link. Resolves relative
    // paths against the target's root_dir so callers can pass bare filenames.

    if ( ctx.file_path )
    {
        if ( !target ) { printf( ORB_INDENT "[orb error] -file requires -target\n" ); return 1; }

        const char* effective_file = ctx.file_path;
        char resolved_file[ PATH_MAX ];
        bool is_abs = platform_is_abs_path( ctx.file_path );
        if ( !is_abs && target->root_dir )
        {
            char combined[ PATH_MAX ];
            snprintf( combined, sizeof( combined ), "%s" PATH_SEP "%s", target->root_dir, ctx.file_path );
            if ( !platform_fullpath( resolved_file, combined, sizeof( resolved_file ) ) )
                snprintf( resolved_file, sizeof( resolved_file ), "%s", combined );
            effective_file = resolved_file;
        }

        const char* base_name = effective_file;
        for ( const char* p = effective_file; *p; ++p )
            if ( *p == '\\' || *p == '/' ) base_name = p + 1;

        char obj_dir[ PATH_MAX ];
        snprintf( obj_dir, sizeof( obj_dir ), "%s" PATH_SEP "%s" PATH_SEP "%s", g_build_dir, g_int_dir, target->name );
        char gen_dir[ PATH_MAX ];
        snprintf( gen_dir, sizeof( gen_dir ), "%s" PATH_SEP "%s", g_build_dir, g_gen_dir );

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

    // --- Command: BUILD (serial single-target via VS, or parallel via CLI) ---
    //
    // -no-deps: VS invokes build_tool.exe per-project with MSBuild managing dep ordering.
    //           Skip the scheduler; build only the named target.
    // otherwise: parallel scheduler over the full target closure.

    if ( ctx.skip_deps )
    {
        if ( !target ) { printf( ORB_INDENT "[orb error] -no-deps requires -target\n" ); return 1; }

        bool was_skipped = false;
        if ( !build_target( &ctx, target, &was_skipped ) )
        {
            printf( ORB_BANNER "\n[ %s: FAILED ]\n", target_upper );
            return 1;
        }

        bool show_result  = ( g_out_flags & ORB_OUT_TARGET_RESULT );
        bool show_skipped = was_skipped && ( g_out_flags & ORB_OUT_SUMMARY_COMPILE );
        if ( show_result || show_skipped )
            printf( ORB_INDENT "[orb %s] %s\n", was_skipped ? "skipped" : "completed", target->name );
    }
    else
    {
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
