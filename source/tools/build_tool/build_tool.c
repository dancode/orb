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
//
// All build outputs land under <g_build_dir>/. The .sln/.vcxproj files also
// live here (see build_tool_gen.c) so VS treats the directory as the
// project root for its IntelliSense and intermediate caches.
static const char* g_build_dir       = "build_new";      // Root for intermediate/generated files.
static const char* g_int_dir         = "obj";            // Sub-folder for .obj files (per-target).
static const char* g_gen_dir         = "generated";      // Sub-folder for reflection-generated .c/.h.


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

/*============================================================================================*/
// --- Core Build Logic ---

/**
 * build_clean()
 *
 * Two modes:
 *
 *   Per-target (target != NULL): removes only that target's artifacts —
 *   bin/<name>.{lib,dll,exe,exp,pdb}, obj/<name>/, and any generated
 *   reflection files. Called from each VS .vcxproj's NMakeCleanCommandLine
 *   so a solution rebuild cleans each project independently rather than
 *   wiping the whole bin/ tree before every project.
 *
 *   Global (target == NULL): wipes all intermediates and artifacts. Skips
 *   is_tool executables (build_reflect, build_tool) so tools survive a
 *   full clean — they are rebuilt on demand by our dep resolution, not
 *   by VS, so deleting them would leave no path to recreate them.
 */
void
build_clean( target_info_t* target )
{
#if defined( _WIN32 )
    char cmd[ BT_PATH_MAX * 2 ];

    if ( target )
    {
        // One-line per-target clean. Sub-commands run silently; we print a
        // single summary at the end so MSBuild output stays parseable.
        const char* ext = ( target->type == TARGET_STATIC_LIB )  ? "lib" :
                          ( target->type == TARGET_DYNAMIC_LIB ) ? "dll" : "exe";
        snprintf( cmd, sizeof( cmd ), "del /q bin\\%s.%s >nul 2>nul", target->name, ext );
        build_run_cmd_quiet( cmd );

        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            snprintf( cmd, sizeof( cmd ), "del /q bin\\%s.lib >nul 2>nul", target->name );
            build_run_cmd_quiet( cmd );
            snprintf( cmd, sizeof( cmd ), "del /q bin\\%s.exp >nul 2>nul", target->name );
            build_run_cmd_quiet( cmd );
        }

        snprintf( cmd, sizeof( cmd ), "del /q bin\\%s_*.pdb >nul 2>nul", target->name );
        build_run_cmd_quiet( cmd );

        snprintf( cmd, sizeof( cmd ), "rd /s /q %s\\%s\\%s >nul 2>nul", g_build_dir, g_int_dir, target->name );
        build_run_cmd_quiet( cmd );

        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            snprintf( cmd, sizeof( cmd ), "del /q %s\\%s\\%s.generated.c >nul 2>nul", g_build_dir, g_gen_dir, rname );
            build_run_cmd_quiet( cmd );
            snprintf( cmd, sizeof( cmd ), "del /q %s\\%s\\%s.generated.h >nul 2>nul", g_build_dir, g_gen_dir, rname );
            build_run_cmd_quiet( cmd );
        }

        printf( ORB_INDENT "[orb clean] %s -- bin\\%s.%s, %s\\%s\\%s%s\n",
                target->name, target->name, ext, g_build_dir, g_int_dir, target->name,
                target->has_reflect ? " (+reflect)" : "" );
    }
    else
    {
        // Global wipe. Same noise-suppression pattern: every del runs silently,
        // one summary line at the end.
        snprintf( cmd, sizeof( cmd ), "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_int_dir );
        build_run_cmd_quiet( cmd );
        snprintf( cmd, sizeof( cmd ), "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_gen_dir );
        build_run_cmd_quiet( cmd );

        build_run_cmd_quiet( "del /s /q bin\\*.pdb >nul 2>nul" );
        build_run_cmd_quiet( "del /s /q bin\\*.lib >nul 2>nul" );
        build_run_cmd_quiet( "del /s /q bin\\*.dll >nul 2>nul" );
        build_run_cmd_quiet( "del /s /q bin\\*.exp >nul 2>nul" );

        // Delete executables only for non-tool targets. is_tool executables
        // (build_reflect, build_tool) are rebuilt by our dep resolution and
        // have no VS project to rebuild them after a clean, so leave them.
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( g_targets[ i ].type == TARGET_EXECUTABLE && !g_targets[ i ].is_tool )
            {
                snprintf( cmd, sizeof( cmd ), "del /q bin\\%s.exe >nul 2>nul", g_targets[ i ].name );
                build_run_cmd_quiet( cmd );
            }
        }

        printf( ORB_INDENT "[orb clean] all -- bin\\*, %s\\{%s,%s}\\*\n",
                g_build_dir, g_int_dir, g_gen_dir );
    }
    return;
#else
    char cmd[ BT_PATH_MAX ];
    if ( target )
    {
        const char* ext = ( target->type == TARGET_STATIC_LIB )  ? "lib" :
                          ( target->type == TARGET_DYNAMIC_LIB ) ? "so"  : "";
        snprintf( cmd, sizeof( cmd ), "rm -f bin/%s.%s", target->name, ext );
        build_run_cmd( cmd );
        snprintf( cmd, sizeof( cmd ), "rm -rf %s/%s/%s", g_build_dir, g_int_dir, target->name );
        build_run_cmd( cmd );
    }
    else
    {
        snprintf( cmd, sizeof( cmd ), "rm -rf bin %s/%s %s/%s", g_build_dir, g_int_dir, g_build_dir, g_gen_dir );
        build_run_cmd( cmd );
        build_run_cmd( "mkdir bin" );
        snprintf( cmd, sizeof( cmd ), "mkdir -p %s/%s", g_build_dir, g_int_dir );
        build_run_cmd( cmd );
        snprintf( cmd, sizeof( cmd ), "mkdir -p %s/%s", g_build_dir, g_gen_dir );
        build_run_cmd( cmd );
    }
#endif
    // printf( "Clean complete.\n" );
}

/*============================================================================================*/

/**
 * build_target()
 *
 * The main worker function. Builds one target, optionally recursing into
 * its dependencies first. Idempotent: a fully up-to-date target returns
 * true without invoking any compiler. Phases run in this order:
 *
 *   0. Dependency resolution (skipped if ctx->skip_deps)
 *   1. Path preparation (obj_dir, out_path, etc.)
 *   2. Up-to-date check (artifact mtime vs unit / dep-lib / header mtimes)
 *   3. Output directory creation
 *   4. Locked-file management (.exe -> .exe.old rename trick)
 *   5. Reflection codegen (only if target->has_reflect)
 *   6. Compile + link
 *
 * Concurrency: from step 1 onward a per-target named mutex is held, so
 * two build_tool.exe invocations (or two scheduler workers — see
 * build_tool_sched.c) targeting the same name will serialize here.
 * Independent targets run fully in parallel because their mutex names differ.
 */
bool
build_target( build_context_t* ctx, target_info_t* target )
{
    // --- 0. Dependency Resolution ---
    //
    // Recurse into link deps and tool deps before building this target.
    // Skipped entirely when -no-deps is set:
    //  - VS solution builds (-no-deps from the .vcxproj) let MSBuild's
    //    scheduler honor ProjectDependencies and queue dep projects first.
    //  - The CLI parallel scheduler (build_run_parallel) sets skip_deps=true
    //    on each worker call because the scheduler itself is the dep authority.
    // Recursing in either context would mean every dep gets walked once per
    // dependent, and multiple processes/threads would race shared outputs.

    if ( !ctx->skip_deps )
    {
        // Link Dependencies — VS manages these via ProjectDependencies when
        // -no-deps is set. Skip here to avoid racing VS's parallel scheduler.
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            target_info_t* dep = NULL;
            for ( int j = 0; j < g_target_count; ++j )
            {
                if ( strcmp( g_targets[ j ].name, target->deps[ i ] ) == 0 ) { dep = &g_targets[ j ]; break; }
            }
            if ( !dep ) { printf( ORB_INDENT "[orb error] '%s' depends on unknown target '%s'\n", target->name, target->deps[ i ] ); return false; }
            if ( !build_target( ctx, dep ) ) return false;
        }
    }

    // Tool Dependencies — always our responsibility regardless of -no-deps.
    // VS has no visibility into tool executables not listed in the solution,
    // so we must always check and rebuild them ourselves. build_target is
    // idempotent; the up-to-date check short-circuits when nothing changed.
    for ( int i = 0; target->tool_deps[ i ]; ++i )
    {
        target_info_t* tool = NULL;
        for ( int j = 0; j < g_target_count; ++j )
        {
            if ( strcmp( g_targets[ j ].name, target->tool_deps[ i ] ) == 0 ) { tool = &g_targets[ j ]; break; }
        }
        if ( !tool ) { printf( ORB_INDENT "[orb error] '%s' has unknown tool dep '%s'\n", target->name, target->tool_deps[ i ] ); return false; }
        if ( !build_target( ctx, tool ) ) return false;
    }

    // Implicit reflect tool dep — same always-rebuild guarantee.
    if ( target->has_reflect )
    {
        target_info_t* refl_tool = NULL;
        for ( int j = 0; j < g_target_count; ++j )
        {
            if ( g_targets[ j ].is_reflect_tool ) { refl_tool = &g_targets[ j ]; break; }
        }
        if ( !refl_tool ) { printf( ORB_INDENT "[orb error] '%s' needs reflection but no is_reflect_tool target is registered\n", target->name ); return false; }
        if ( !build_target( ctx, refl_tool ) ) return false;
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
    //
    // Three independent freshness tests, each guarded by the running
    // `up_to_date` flag so we short-circuit out of expensive walks. A miss
    // on ANY test forces a full rebuild; we don't try to be clever about
    // partial recompilation because unity builds make per-file rebuilds
    // meaningless anyway (one TU touches everything).

    __time64_t out_mtime = build_get_mtime( out_path );
    bool up_to_date = ( out_mtime != 0 );  // No artifact = first build = rebuild.

    // Test A: any explicit translation unit newer than the artifact?
    if ( up_to_date )
    {
        for ( int i = 0; target->units[ i ]; ++i )
        {
            char src_path[ BT_PATH_MAX ];
            snprintf( src_path, sizeof( src_path ), "%s/%s", target->root_dir, target->units[ i ] );
            if ( build_get_mtime( src_path ) > out_mtime ) { up_to_date = false; break; }
        }
    }

    // Test B: any linked dep .lib newer than the artifact? Catches the case
    // where a sibling target rebuilt and we need to re-link against it.
    if ( up_to_date )
    {
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            char dep_path[ BT_PATH_MAX ];
            snprintf( dep_path, sizeof( dep_path ), "bin\\%s.lib", target->deps[ i ] );
            if ( build_get_mtime( dep_path ) > out_mtime ) { up_to_date = false; break; }
        }
    }

    // Test C: header dependency check. The previous successful compile
    // wrote every #included header path into <obj_dir>/_deps.txt (parsed out
    // of cl.exe's /showIncludes output — see build_target_compile and
    // build_run_cmd_capture_deps). On this pass we replay that list and
    // rebuild if any listed header is newer than the artifact.
    //
    // No _deps.txt = no recorded header set = we have to assume the worst
    // and rebuild. This is correct on the very first build and after a
    // clean; it auto-recovers on the next pass.
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
                // Strip trailing CR/LF from fgets so the path round-trips
                // through build_get_mtime cleanly.
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

    // All three tests passed → skip compile + link entirely. We still ran
    // through the lock-and-prepare phase so concurrent callers got serialized
    // and observed the artifact as fully written.
    if ( up_to_date ) { result = true; goto cleanup; }

    // --- 3. Directory Creation ---
    //
    // Make sure every directory we're about to write into exists. _access()
    // probes are cheap and let us skip the mkdir spawn when the dir is
    // already present (the common case after the first build).

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
    //
    // Windows refuses to overwrite a running .exe (sharing violation), but
    // it WILL let you rename one. So if we're about to relink an EXE that
    // might be in use (e.g. a sandbox the user just ran from VS), shove
    // the old image aside to <name>.exe.old first. If the subsequent compile
    // or link fails we restore it; if everything succeeds the .old file is
    // overwritten on the next build cycle.

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
    //
    // Generate the rs_-system codegen for targets that opt in via
    // has_reflect=true. build_reflect.exe scans root_dir for annotated
    // types and writes <gen_dir>/<rname>.generated.{c,h}; the
    // generated .c is then appended to the compile step's input list.
    // reflect_name overrides the stem; falls back to target->name when NULL.

    if ( target->has_reflect )
    {
        target_info_t* refl_tool = NULL;
        for ( int j = 0; j < g_target_count; ++j )
        {
            if ( g_targets[ j ].is_reflect_tool ) { refl_tool = &g_targets[ j ]; break; }
        }
        if ( !refl_tool )
        {
            printf( ORB_INDENT "[orb error] no is_reflect_tool target registered — cannot generate reflection for '%s'\n", target->name );
            if ( renamed ) rename( old_path, exe_path );
            result = false;
            goto cleanup;
        }
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        if ( g_out_flags & ORB_OUT_REFLECT )
        {
            // Route to the per-target log when inside a parallel worker so the
            // reflect line lands with the rest of the target's output.
            const char* _lp = sched_log_path();
            FILE* _lf = _lp ? fopen( _lp, "a" ) : NULL;
            fprintf( _lf ? _lf : stdout, ORB_INDENT "[orb reflect] %s\n", rname );
            if ( _lf ) fclose( _lf );
        }
        char refl_cmd[ BT_PATH_MAX * 2 ];
        snprintf( refl_cmd, sizeof( refl_cmd ), "bin\\%s.exe %s %s %s", refl_tool->name, target->root_dir, gen_dir, rname );
        if ( build_run_cmd( refl_cmd ) != 0 )
        {
            if ( renamed ) rename( old_path, exe_path );
            result = false;
            goto cleanup;
        }
    }

    // --- 6. Compile & Link ---
    //
    // Two phases. Compile emits .obj files into obj_dir and records the
    // header dependency set into _deps.txt. Link/archive ties the .obj
    // files into the final .lib/.dll/.exe artifact. Either failing
    // restores the renamed-aside .exe.old so the user is not left with a
    // gap where the binary used to be.

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
    // Always release the per-target mutex on the way out, regardless of
    // success/failure/short-circuit, so concurrent callers can proceed.
    build_unlock_target( target_lock );
    return result;
}

/*============================================================================================*/
// --- Main Entry ---
//
// Recognized arguments:
//   -clean / clean          Wipe build outputs and exit.
//   -gen   / gen            Regenerate .sln/.vcxproj and exit.
//   -target <name>          Restrict the build to one target's closure.
//   -no-deps                Build only the target itself, no dep recursion
//                           (set by VS .vcxproj files; do not use on the CLI
//                           unless you know exactly what you're doing).
//   -config <Debug|Release> Pick build config (default Debug).
//   release                 Shortcut for -config Release.
//   clang                   Use clang-cl instead of cl.exe.
//   -j N                    Worker thread count; 0/unset = auto-detect.

int
main( int argc, char** argv )
{
    build_context_t ctx = { 0 };
    ctx.config = CONFIG_DEBUG;

    bool  should_clean = false;
    bool  should_gen   = false;
    char* target_name  = NULL;
    int   j_threads    = 0;   // 0 → auto-detect from CPU count.

    // --- Arg parsing ---
    // Order-independent; the loop just sets flags. Unknown args are silently
    // ignored (intentional — keeps backward compat with VS External Tools
    // call sites that might pass legacy positional args).
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
            for ( int i = 0; i < g_target_count; ++i )
            {
                if ( _stricmp( g_targets[ i ].name, target_name ) == 0 ) { clean_target = &g_targets[ i ]; break; }
            }
            if ( !clean_target ) { printf( ORB_INDENT "[orb error] unknown target '%s'\n", target_name ); return 1; }
        }
        build_clean( clean_target );
        return 0;
    }
    if ( should_gen ) { build_gen_projects(); return 0; }

    // Startup banner — target is the headline; sub-line carries config + args.
    char target_upper[ 64 ] = "ALL";
    if ( target_name )
    {
        int k = 0;
        for ( ; target_name[ k ] && k < (int)sizeof( target_upper ) - 1; ++k )
            target_upper[ k ] = (char)toupper( (unsigned char)target_name[ k ] );
        target_upper[ k ] = '\0';
    }
    printf( ORB_BANNER "[ orb build: %s ]\n", target_upper );
    printf( ORB_INDENT "%s | %s | args:",
            ctx.config == CONFIG_DEBUG ? "Debug" : "Release",
            ctx.is_clang ? "Clang" : "MSVC" );
    for ( int i = 1; i < argc; ++i ) printf( " %s", argv[ i ] );
    printf( "\n\n" );


    // Make cl.exe / link.exe / lib.exe callable. Idempotent fast-path when
    // we're already inside a Developer Command Prompt (or VS-launched shell).
    build_setup_vc_env();

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
        if ( !target ) { printf( ORB_INDENT "[orb error] unknown target '%s'\n", target_name ); return 1; }
    }

    if ( ctx.skip_deps )
    {
        if ( !target ) { printf( ORB_INDENT "[orb error] -no-deps requires -target\n" ); return 1; }
        if ( !build_target( &ctx, target ) )
        {
            printf( ORB_BANNER "[ FAILED: %s ]\n", target_upper );
            return 1;
        }
    }
    else
    {
        if ( !build_run_parallel( &ctx, target, j_threads ) )
        {
            printf( ORB_BANNER "[ FAILED: %s ]\n", target_upper );
            return 1;
        }
    }

    // printf( ORB_BANNER "[ orb done: %s ]\n", target_upper );
    return 0;
}

// clang-format on
/*============================================================================================*/
