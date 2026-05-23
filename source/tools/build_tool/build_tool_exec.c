/*==============================================================================================

    build_tool_exec.c -- Build execution: artifact cleanup and per-target build driver.

    Two public entry points called from main() and the parallel scheduler:

        build_clean()  -- Wipe artifacts for one target or all non-tool targets.
        build_target() -- Dep resolution, up-to-date check, reflect codegen, compile + link.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    -- Quiet Delete Helper -- 
    
    Designed to keep the build_clean code readable and the terminal output tidy. 
    
        Ex: format a "del /q ... >nul 2>nul" command and run it silently.
    
    Appending >nul 2>nul to the shell command tells the Windows shell to discard 
    both standard output and error messages.

    It calls build_run_cmd_quiet, so even if the del command returns a "file not found" 
    error code, the build tool doesn't log it.
    
    build_clean() invokes this for every artifact category it deletes.
    e.g. (bin/<name>.lib, .dll, .exe, .pdb, .exp, .generated.{c,h}, ...); 
==============================================================================================*/

static void
del_q( const char* fmt, ... )
{
    char cmd[ BT_PATH_MAX * 2 ];
    va_list args; va_start( args, fmt ); 
    vsnprintf( cmd, sizeof( cmd ), fmt, args ); 
    va_end( args );

    build_run_cmd_quiet( cmd );    
}

/*==============================================================================================
    --- Build Clean ---
   
    Two modes: per-target + global.

    Per-target (target != NULL): removes only that target's artifacts — bin/<name>.
    {lib,dll,exe,exp,pdb}, obj/<name>/, and any generated reflection files. 

    Called from each VS .vcxproj's invoke of NMakeCleanCommandLine so a solution 
    rebuild cleans each project independently rather than wiping the whole 
    bin/ tree before every project.

    Global (target == NULL): wipes all intermediates and artifacts. 

    Skips "is_tool" executables (build_reflect, build_tool) so tools survive a 
    full clean — they are rebuilt on demand by our dep resolution, not by VS, 
    so deleting them would leave no path to recreate them.

==============================================================================================*/
void
build_clean( target_info_t* target )
{
    if ( target )
    {
        // One-line per-target clean. Sub-commands run silently; we print a
        // single summary at the end so MSBuild output stays parseable.

        // delete the main target type artifact:
        const char* ext = ( target->type == TARGET_STATIC_LIB )  ? "lib"
                        : ( target->type == TARGET_DYNAMIC_LIB ) ? "dll" 
                        :                                          "exe";

        del_q( "del /q bin\\%s.%s >nul 2>nul", target->name, ext );

        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            // Cover both monolithic (.lib primary) and dynamic (.dll primary)
            // outputs plus the import .exp. del /q is a no-op when the file
            // doesn't exist, so listing all three is always safe.
            del_q( "del /q bin\\%s.lib >nul 2>nul", target->name );
            del_q( "del /q bin\\%s.dll >nul 2>nul", target->name );
            del_q( "del /q bin\\%s.exp >nul 2>nul", target->name );
        }
         
        del_q( "del /q bin\\%s_*.pdb >nul 2>nul", target->name );
        del_q( "rd /s /q %s\\%s\\%s >nul 2>nul", g_build_dir, g_int_dir, target->name );
        
        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            del_q( "del /q %s\\%s\\%s.generated.c >nul 2>nul", g_build_dir, g_gen_dir, rname );
            del_q( "del /q %s\\%s\\%s.generated.h >nul 2>nul", g_build_dir, g_gen_dir, rname );
        }

        printf( ORB_INDENT "[orb clean] %s -- bin\\%s.%s, %s\\%s\\%s%s\n", target->name, target->name, ext,
                g_build_dir, g_int_dir, target->name, target->has_reflect ? " (+reflect)" : "" );
    }
    else
    {
        // Global wipe with the same noise-suppression pattern: every del runs silently,
        // we supress the noise and output one summary line at the end.
        del_q( "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_int_dir );
        del_q( "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_gen_dir );

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
                del_q( "del /q bin\\%s.exe >nul 2>nul", g_targets[ i ].name );
        }
        printf( ORB_INDENT "[orb clean] all -- bin\\*, %s\\{%s,%s}\\*\n", 
                g_build_dir, g_int_dir, g_gen_dir );
    }
}

/*============================================================================================*/

/**
 * build_target_compile_only()
 *
 * Compile all unity units for a target without running the link/archive step.
 * Called from the VS Ctrl+F7 path (-compile-only flag) via NMakeCompileFileCommandLine.
 * For unity builds, "compile single file" is semantically "compile the whole unity TU
 * with no link" — there is no meaningful per-file granularity below the unity root.
 */
bool
build_target_compile_only( build_context_t* ctx, target_info_t* target )
{
    char obj_dir[ BT_PATH_MAX ];
    snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
    char gen_dir[ BT_PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );

    // Ensure intermediate directories exist; bin/ is not needed (no artifact produced).
    char int_root[ BT_PATH_MAX ];
    snprintf( int_root, sizeof( int_root ), "%s\\%s", g_build_dir, g_int_dir );
    ensure_dir( g_build_dir );
    ensure_dir( int_root );
    ensure_dir( gen_dir );
    ensure_dir( obj_dir );

    return build_target_compile( ctx, target, obj_dir, gen_dir );
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
build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped )
{
    if ( out_skipped )
        *out_skipped = false;
    target_info_t* refl_tool = NULL;    // Located in step 0; reused in step 5.

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
            target_info_t* dep = find_target( target->deps[ i ] );
            if ( !dep )
            {
                printf( ORB_INDENT "[orb error] '%s' depends on unknown target '%s'\n", target->name,
                        target->deps[ i ] );
                return false;
            }
            if ( !build_target( ctx, dep, NULL ) )
                return false;
        }
    }

    // Tool Dependencies — always our responsibility regardless of -no-deps.
    // VS has no visibility into tool executables not listed in the solution,
    // so we must always check and rebuild them ourselves. build_target is
    // idempotent; the up-to-date check short-circuits when nothing changed.
    for ( int i = 0; target->tool_deps[ i ]; ++i )
    {
        target_info_t* tool = find_target( target->tool_deps[ i ] );
        if ( !tool )
        {
            printf( ORB_INDENT "[orb error] '%s' has unknown tool dep '%s'\n", target->name, target->tool_deps[ i ] );
            return false;
        }
        if ( !build_target( ctx, tool, NULL ) )
            return false;
    }

    // Implicit reflect tool dep — same always-rebuild guarantee.
    if ( target->has_reflect )
    {
        refl_tool = find_reflect_tool();
        if ( !refl_tool )
        {
            printf( ORB_INDENT "[orb error] '%s' needs reflection but no is_reflect_tool target is registered\n",
                    target->name );
            return false;
        }
        if ( !build_target( ctx, refl_tool, NULL ) )
            return false;
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

    // In monolithic mode a dynamic lib produces a .lib, not a .dll.
    const char* ext = ( target->type == TARGET_STATIC_LIB )    ? ".lib"
                      : ( target->type == TARGET_DYNAMIC_LIB ) ? ( ctx->is_monolithic ? ".lib" : ".dll" )
                                                               : ".exe";

    char        out_path[ BT_PATH_MAX ];
    snprintf( out_path, sizeof( out_path ), "bin\\%s%s", target->name, ext );

    // --- 2. Up-to-Date Check ---
    //
    // Four independent freshness tests (A, B, C, D below), each guarded by
    // the running `up_to_date` flag so we short-circuit out of expensive
    // walks. A miss on ANY test forces a full rebuild; we don't try to be
    // clever about partial recompilation because unity builds make per-file
    // rebuilds meaningless anyway (one TU touches everything).
    //
    //   A. any unit's source file newer than the artifact?
    //   B. any link-dep's .lib newer than the artifact?
    //   C. did the build config (Debug/Release) flip since last build?
    //   D. any tracked header newer than the artifact?

    __time64_t out_mtime  = build_get_mtime( out_path );
    bool       up_to_date = ( out_mtime != 0 ) && !ctx->force_rebuild;    // No artifact = first build = rebuild.

    // Test A: any explicit translation unit newer than the artifact?
    if ( up_to_date )
    {
        for ( int i = 0; target->units[ i ]; ++i )
        {
            char src_path[ BT_PATH_MAX ];
            snprintf( src_path, sizeof( src_path ), "%s/%s", target->root_dir, target->units[ i ] );
            if ( build_get_mtime( src_path ) > out_mtime )
            {
                up_to_date = false;
                break;
            }
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
            if ( build_get_mtime( dep_path ) > out_mtime )
            {
                up_to_date = false;
                break;
            }
        }
    }

    // Test C: config change check. If the last successful build used a different
    // config (Debug vs Release), the artifact is stale even though no source
    // file changed. _config.txt is written after every successful compile+link.
    // Missing file = first build or post-clean = must rebuild.
    if ( up_to_date )
    {
        const char* current_config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";
        char        config_marker[ BT_PATH_MAX ];
        snprintf( config_marker, sizeof( config_marker ), "%s\\_config.txt", obj_dir );
        FILE* cf = fopen( config_marker, "r" );
        if ( !cf )
        {
            up_to_date = false;
        }
        else
        {
            char stored[ 16 ] = { 0 };
            fgets( stored, sizeof( stored ), cf );
            fclose( cf );
            strip_eol( stored );
            if ( strcmp( stored, current_config ) != 0 )
                up_to_date = false;
        }
    }

    // Test D: header dependency check. The previous successful compile
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
                // Strip the newline fgets leaves on the path so it round-trips
                // through build_get_mtime cleanly.
                strip_eol( header_path );
                if ( header_path[ 0 ] == '\0' )
                    continue;

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
    if ( up_to_date )
    {
        if ( out_skipped )
            *out_skipped = true;
        result = true;
        goto cleanup;
    }

    // --- 3. Directory Creation ---
    //
    // Make sure every directory we're about to write into exists. _access()
    // probes are cheap and let us skip the mkdir spawn when the dir is
    // already present (the common case after the first build).

    char int_root[ BT_PATH_MAX ];
    snprintf( int_root, sizeof( int_root ), "%s\\%s", g_build_dir, g_int_dir );

    ensure_dir( "bin" );
    ensure_dir( g_build_dir );
    ensure_dir( int_root );
    ensure_dir( gen_dir );
    ensure_dir( obj_dir );

    // --- 4. Locked File Management ---
    //
    // Windows refuses to overwrite a running .exe (sharing violation), but
    // it WILL let you rename one. So if we're about to relink an EXE that
    // might be in use (e.g. a sandbox the user just ran from VS), shove
    // the old image aside to <name>.exe.old first. If the subsequent compile
    // or link fails we restore it; if everything succeeds the .old file is
    // overwritten on the next build cycle.

    char exe_path[ BT_PATH_MAX ] = { 0 };
    char old_path[ BT_PATH_MAX ] = { 0 };
    bool renamed                 = false;

    if ( target->type == TARGET_EXECUTABLE )
    {
        snprintf( exe_path, sizeof( exe_path ), "bin\\%s.exe", target->name );
        snprintf( old_path, sizeof( old_path ), "bin\\%s.exe.old", target->name );
        if ( _access( exe_path, 0 ) == 0 )
        {
            remove( old_path );
            if ( rename( exe_path, old_path ) == 0 )
                renamed = true;
        }
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
        // refl_tool was located and built in step 0; NULL is impossible here
        // because step 0 returned false when no reflect tool was registered.
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        if ( g_out_flags & ORB_OUT_REFLECT )
        {
            // Route to the per-target log when inside a parallel worker so the
            // reflect line lands with the rest of the target's output.
            const char* _lp = sched_log_path();
            FILE*       _lf = _lp ? fopen( _lp, "a" ) : NULL;
            fprintf( _lf ? _lf : stdout, ORB_INDENT "[orb reflect] %s\n", rname );
            if ( _lf )
                fclose( _lf );
        }
        char refl_cmd[ BT_PATH_MAX * 2 ];
        snprintf( refl_cmd, sizeof( refl_cmd ), "bin\\%s.exe %s %s %s", refl_tool->name, target->root_dir,
                  gen_dir, rname );
        if ( build_run_cmd( refl_cmd ) != 0 )
        {
            if ( renamed )
                rename( old_path, exe_path );
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
        if ( renamed )
            rename( old_path, exe_path );
        result = false;
        goto cleanup;
    }

    if ( !build_target_link( ctx, target, obj_dir ) )
    {
        if ( renamed )
            rename( old_path, exe_path );
        result = false;
        goto cleanup;
    }

    // Record the config used for this build so the next incremental check can
    // detect a Debug<->Release switch even when no source file has changed.
    {
        char config_marker[ BT_PATH_MAX ];
        snprintf( config_marker, sizeof( config_marker ), "%s\\_config.txt", obj_dir );
        FILE* cf = fopen( config_marker, "w" );
        if ( cf )
        {
            fprintf( cf, "%s\n", ctx->config == CONFIG_DEBUG ? "Debug" : "Release" );
            fclose( cf );
        }
    }

cleanup:
    // Always release the per-target mutex on the way out, regardless of
    // success/failure/short-circuit, so concurrent callers can proceed.
    build_unlock_target( target_lock );
    return result;
}

/*============================================================================================*/
// clang-format on