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

        win.c            -- 00a platform layer: file I/O, CRT wrappers
        win_thread.c     -- 00b platform threading: mutex, cond, TLS, threads
        win_spawn.c      -- 00c platform process spawning
        win_toolchain.c  -- 00d compiler/linker flag sets (MSVC vs GCC/Clang)
        posix_*.c        -- POSIX equivalents of the above for Linux/macOS

        00_util.c         -- pre-main utilities: deps graph, validate_targets, startup_banner
        01_prim.c         -- shared: cmd_buf, file locks (pure primitives, no deps)
        02_data.c         -- g_targets[] / g_solutions[] dynamic pools + lookup helpers
        03_registry.c     -- "orb.targets" text-file parser; appends to 02_data pools
        04_env.c          -- VS environment discovery and vcvars import
        05_log.c          -- stateless output formatters (print_section, etc.)
        06_spawn.c        -- child process spawning, /showIncludes capture
        07_compile.c      -- cl.exe command assembly and execution
        08_link.c         -- link.exe / lib.exe command assembly and execution
        09_exec.c         -- build_target() orchestration
        10_sched.c        -- topological worker-pool parallel scheduler
        11_clean.c        -- -clean command: per-target or global artifact wipe
        12_gen_nmake.c    -- -gen command: NMake-style .sln/.vcxproj (build_tool owns build)
        12_gen_json.c     -- -gen command: compile_commands.json (clangd / LSP tools)
        12_gen_vscode.c   -- -gen command: .vscode/tasks.json (VS Code build tasks)
        12_gen_msbuild.c  -- -gen_ms command: native MSBuild projects (full EDG IntelliSense)
        test.c            -- debug arg injection from build_tool_debug.args
        00_util.c         -- pre-main utilities: deps graph, validate_targets, print_startup_banner

==============================================================================================*/
// clang-format off

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "tools/build_tool/build_tool.h"

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

    ORB_BANNER: indent for top-level orb lines  e.g.  "      [orb build: X]"
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

static const char* g_build_dir      = BUILD_DIR;    // root for VS project files + intermediates.
static const char* g_int_dir        = "obj";        // sub-folder: per-target .obj files.
static const char* g_gen_dir        = "generated";  // sub-folder: reflection-generated .c/.h.

/*==============================================================================================
    --- Output Flags ---

    Process-global verbosity mask. Set once in main() from CLI args; all
    unity-included modules read it via the extern declared in build_tool.h.
==============================================================================================*/

static out_flags_t g_out_flags      = ORB_OUT_DEFAULT;

static bool        g_include_track  = true;         // Use up-to-date tracking via headers.
static bool        g_use_rsp        = true;         // Use overflow prevention.
static bool        g_gen_fwd_compat = true;         // -gen: emit stdcpp20 alongside stdc11.
                                                    // (suppress designated-initializer squiggles)

/*==============================================================================================
    --- ANSI Color Strings ---

    Initialized to empty strings; set to real escape codes in main() when the terminal
    supports ENABLE_VIRTUAL_TERMINAL_PROCESSING. All modules use these globals so color
    degrades gracefully when redirected to a file or pipe.
==============================================================================================*/

static const char* g_clr_reset  = "";
static const char* g_clr_red    = "";
static const char* g_clr_green  = "";
static const char* g_clr_yellow = "";
static const char* g_clr_dim    = "";
static const char* g_clr_bold   = "";

/*==============================================================================================
    --- Unity Include Chain ---

    Execution order == include order. Reading top-to-bottom traces the program
    from startup through each command to the terminal dispatch in main().

    Forward declarations for functions defined in later modules but called by
    earlier ones. One declaration here beats one per call site.
==============================================================================================*/

/* Defined in 10_sched.c. Returns the active worker's per-thread log path, or NULL
   when not inside a parallel worker (serial / main-thread path). */
const char* sched_log_path( void );

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
#include "build_tool_03_registry.c"         // 03 orb.targets text-file parser
#include "build_tool_04_env.c"              // 04 vcvars setup
#include "build_tool_05_log.c"              // 05 output formatting
#include "build_tool_06_spawn.c"            // 06 child process spawning
#include "build_tool_07_compile.c"          // 07 compile command
#include "build_tool_08_link.c"             // 08 link command
#include "build_tool_09_exec.c"             // 09 build_target orchestration
#include "build_tool_10_sched.c"            // 10 parallel scheduler
#include "build_tool_11_clean.c"            // 11 -clean command
#include "build_tool_12_gen_manifest.c"     // 12   gen manifest (resolved intent; built before all generators)
#include "build_tool_12_gen_nmake.c"        // 12a -gen command (NMake/Makefile projects)
#include "build_tool_12_gen_json.c"         // 12b -gen command (compile_commands.json)
#include "build_tool_12_gen_vscode.c"       // 12c -gen command (.vscode/tasks.json)
#include "build_tool_12_gen_msbuild.c"      // 12d -gen_ms command (MSBuild projects)
#include "build_tool_test.c"                // debug arg injection (debug builds only)
#include "build_tool_00_util.c"             // pre-main utilities: deps graph, validate_targets, print_startup_banner

/*==============================================================================================
    --- Main Entry ---

    Run with -help or -h for the full argument reference.

==============================================================================================*/

int
main( int argc, char** argv )
{
    // --- Debug arg injection ---

    build_tool_debug_inject( &argc, &argv );

    // --- ANSI color: enable once; all modules read g_clr_* ---

    if ( platform_enable_ansi_color() )
    {
        g_clr_reset  = "\033[0m";
        g_clr_red    = "\033[31m";
        g_clr_green  = "\033[32m";
        g_clr_yellow = "\033[33m";
        g_clr_dim    = "\033[2m";
        g_clr_bold   = "\033[1m";
    }

    // --- Context defaults ---

    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;
    ctx.compiler = COMPILE_MSVC;

    // --- Operational Command flags ---

    bool should_help         = false;
    bool should_list         = false;
    bool should_graph        = false;
    bool should_clean        = false;
    bool should_gen          = false;
    bool should_gen_nmake    = false;
    bool should_gen_msbuild  = false;
    bool should_bootstrap    = false;
    int  j_threads           = 0;       // 0 -> auto-detect from CPU count.

    // --- Arg parsing (order-independent flag scan) ---

    for ( int i = 1; i < argc; ++i )
    {
        // utility + project generation
        if ( platform_stricmp( argv[ i ], "-help"             ) == 0 ) should_help = true;
        if ( platform_stricmp( argv[ i ], "-h"                ) == 0 ) should_help = true;
        if ( platform_stricmp( argv[ i ], "-list"             ) == 0 ) should_list = true;
        if ( platform_stricmp( argv[ i ], "-graph"            ) == 0 ) should_graph = true;
        if ( platform_stricmp( argv[ i ], "-bootstrap"        ) == 0 ) should_bootstrap = true;
        if ( platform_stricmp( argv[ i ], "-clean"            ) == 0 ) should_clean = true;
        if ( platform_stricmp( argv[ i ], "-gen"              ) == 0 ) should_gen = true;
        if ( platform_stricmp( argv[ i ], "-gen_nm"           ) == 0 ) should_gen_nmake = true;
        if ( platform_stricmp( argv[ i ], "-gen_ms"           ) == 0 ) should_gen_msbuild = true;
        
        // compile settings
        if ( platform_stricmp( argv[ i ], "-target" ) == 0 && i + 1 < argc ) ctx.target_name = argv[ ++i ];
        if ( platform_stricmp( argv[ i ], "-file"   ) == 0 && i + 1 < argc ) ctx.file_path   = argv[ ++i ];
        if ( platform_stricmp( argv[ i ], "-j"      ) == 0 && i + 1 < argc ) j_threads       = atoi( argv[ ++i ] );
        if ( platform_stricmp( argv[ i ], "-config" ) == 0 && i + 1 < argc )
        {
            if ( platform_stricmp( argv[ ++i ], "release" ) == 0 ) ctx.config = CONFIG_RELEASE;
        }
        if ( platform_stricmp( argv[ i ], "-monolithic"       ) == 0 ) { ctx.is_monolithic = true; }
        if ( platform_stricmp( argv[ i ], "-mono"             ) == 0 ) { ctx.is_monolithic = true; }
        if ( platform_stricmp( argv[ i ], "-release"          ) == 0 ) { ctx.config = CONFIG_RELEASE; }
        if ( platform_stricmp( argv[ i ], "-shipping"         ) == 0 ) { ctx.config = CONFIG_RELEASE; ctx.is_shipping = true; }
        if ( platform_stricmp( argv[ i ], "-clang"            ) == 0 ) { ctx.compiler = COMPILE_CLANG; }
        if ( platform_stricmp( argv[ i ], "-compile-only"     ) == 0 ) { ctx.compile_only = true; }
        if ( platform_stricmp( argv[ i ], "-force"            ) == 0 ) { ctx.force_rebuild = true; }
        if ( platform_stricmp( argv[ i ], "-no-deps"          ) == 0 ) { ctx.skip_deps = true; }
        
        // internal operations (developer)
        if ( platform_stricmp( argv[ i ], "-no-rsp"           ) == 0 ) g_use_rsp = false;
        if ( platform_stricmp( argv[ i ], "-no-fwd-compat"    ) == 0 ) g_gen_fwd_compat = false;
        if ( platform_stricmp( argv[ i ], "-no-include-track" ) == 0 ) g_include_track = false;

        // output verbosity
        if ( platform_stricmp( argv[ i ], "-q"                ) == 0 ) g_out_flags = ORB_OUT_QUIET;
        if ( platform_stricmp( argv[ i ], "-v"                ) == 0 ) g_out_flags = ORB_OUT_VERBOSE;
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

    // --- Command: HELP ---

    if ( should_help ) { print_help(); return 0; }

    // --- Target registry: orb.targets first (sets g_engine_root if 'engine' declared),
    //     then built-ins (uses g_engine_root to set paths and is_external correctly). ---

    bool registry_ok = registry_load( "orb.targets", false );
    init_builtin_targets();

    if ( !registry_ok || !validate_targets() ) return 1;

    // --- Command: LIST ---

    if ( should_list )
    {
        static const char* type_tag[] = { "lib", "dll", "exe" };

        // measure the longest name for alignment
        int max_name = 4;
        for ( int i = 0; i < g_target_count; ++i )
        {
            int n = ( int )strlen( g_targets[ i ].name );
            if ( n > max_name ) max_name = n;
        }

        int local_count    = 0;
        int external_count = 0;
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( g_targets[ i ].is_external ) external_count++;
            else                              local_count++;
        }

        printf( ORB_BANNER "[orb targets] %d local, %d external\n\n",
                local_count, external_count );

        // Local targets first, then external, each group in registration order.
        for ( int pass = 0; pass < 2; ++pass )
        {
            bool printing_external = ( pass == 1 );
            bool printed_header    = false;
            for ( int i = 0; i < g_target_count; ++i )
            {
                const target_info_t* t = &g_targets[ i ];
                if ( t->is_external != printing_external ) continue;

                if ( !printed_header )
                {
                    printf( ORB_INDENT "%s\n", printing_external ? "(external)" : "(local)" );
                    printed_header = true;
                }

                const char* kind = ( t->is_build_tool || t->is_reflect_tool || t->is_tool ) ? " [tool]" : "";
                const char* folder = ( t->virtual_folder && t->virtual_folder[ 0 ] ) ? t->virtual_folder : "";
                printf( ORB_INDENT "  [%s]  %-*s  %s%s\n",
                        type_tag[ t->type ], max_name, t->name, folder, kind );
            }
            if ( printed_header ) printf( "\n" );
        }
        return 0;
    }

    // --- Command: GRAPH ---
    //
    // With -target: prints the dependency tree rooted at that target and the flat
    // topological build order for its closure.
    // Without -target: prints the flat build order for all local targets collectively.

    if ( should_graph )
    {
        static const char* type_tag[] = { "lib", "dll", "exe" };

        target_info_t* root = NULL;
        if ( ctx.target_name )
        {
            root = find_target_icase( ctx.target_name );
            if ( !root )
            {
                printf( ORB_INDENT "[orb error] unknown target '%s'\n", ctx.target_name );
                return 1;
            }
        }

        static deps_topo_t topo;
        memset( &topo, 0, sizeof( topo ) );

        if ( root )
        {
            deps_visit( &topo, root );
        }
        else
        {
            for ( int i = 0; i < g_target_count; ++i )
                if ( !g_targets[ i ].is_external )
                    deps_visit( &topo, &g_targets[ i ] );
        }

        if ( topo.has_cycle )
        {
            printf( ORB_INDENT "[orb error] %s\n", topo.cycle_msg );
            return 1;
        }

        const char* label = root ? root->name : "ALL";
        printf( ORB_BANNER "[orb deps]  %s  (%d in closure)\n", label, topo.count );

        // Tree view: single-target mode only.
        if ( root )
        {
            int shown[ MAX_TARGETS + 4 ] = { 0 };
            int ridx = deps_target_idx( root );
            if ( ridx >= 0 ) shown[ ridx ]++;
            printf( "\n" );
            printf( ORB_INDENT "tree:\n" );
            printf( "%s  %-22s  [%s]\n", ORB_INDENT, root->name, type_tag[ root->type ] );
            deps_tree_children( root, shown, "  " );
        }

        // Flat topological build order (leaves first).
        int num_w = ( topo.count >= 10 ) ? 2 : 1;
        printf( "\n" );
        printf( ORB_INDENT "build order:\n" );
        for ( int i = 0; i < topo.count; ++i )
        {
            target_info_t* t = topo.order[ i ];
            printf( "%s  %*d  [%s]  %s\n",
                    ORB_INDENT, num_w, i + 1, type_tag[ t->type ], t->name );
        }
        printf( "\n" );
        return 0;
    }

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
        if ( !build_target( &ctx, bt, &skipped, NULL ) )
        {
            printf( ORB_BANNER "%s[ BOOTSTRAP: FAILED ]%s\n", g_clr_red, g_clr_reset );
            return 1;
        }
        printf( ORB_BANNER "%s[ BOOTSTRAP: OK ]%s\n", g_clr_green, g_clr_reset );
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

    // --- Command: GENERATE ---

    if ( should_gen || should_gen_nmake || should_gen_msbuild )
    {
        static gen_manifest_t manifest;
        gen_manifest_build( &manifest );

        if ( should_gen || should_gen_nmake )
        {
            build_gen_projects( &manifest );
            build_gen_compile_commands( &manifest );
            build_gen_vscode( &manifest );
        }
        if ( should_gen || should_gen_msbuild )
            build_gen_projects_msbuild( &manifest );

        printf( "\nAll projects generated successfully.\n" );
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
            printf( ORB_BANNER "%s[ %s: FAILED ]%s\n", g_clr_red, target_upper, g_clr_reset );
            return 1;
        }
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

        char obj_dir[ PATH_MAX ];
        snprintf( obj_dir, sizeof( obj_dir ), "%s" PATH_SEP "%s" PATH_SEP "%s", g_build_dir, g_int_dir, target->name );
        char gen_dir[ PATH_MAX ];
        snprintf( gen_dir, sizeof( gen_dir ), "%s" PATH_SEP "%s", g_build_dir, g_gen_dir );

        ensure_dir( "bin" );
        ensure_dir( g_build_dir );
        ensure_dir( obj_dir );

        if ( !build_target_compile_single( &ctx, target, obj_dir, gen_dir, effective_file ) )
        {
            printf( ORB_BANNER "%s[ %s: FAILED ]%s\n", g_clr_red, target_upper, g_clr_reset );
            return 1;
        }
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

        bool     was_skipped = false;
        uint64_t elapsed_ms  = 0;
        if ( !build_target( &ctx, target, &was_skipped, &elapsed_ms ) )
        {
            printf( ORB_BANNER "\n[ %s: FAILED ]\n", target_upper );
            return 1;
        }

        if ( was_skipped && ( g_out_flags & ORB_OUT_SUMMARY_COMPILE ) )
            printf( ORB_INDENT "%s[orb skipped]%s %s\n", g_clr_dim, g_clr_reset, target->name );
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
